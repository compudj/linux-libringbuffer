/*
 * ring_buffer_frontend.c
 *
 * (C) Copyright 2005-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer wait-free buffer synchronization. Producer-consumer and flight
 * recorder (overwrite) modes. See thesis:
 *
 * Desnoyers, Mathieu (2009), "Low-Impact Operating System Tracing", Ph.D.
 * dissertation, Ecole Polytechnique de Montreal.
 * http://www.lttng.org/pub/thesis/desnoyers-dissertation-2009-12.pdf
 *
 * - Algorithm presentation in Chapter 5:
 *     "Lockless Multi-Core High-Throughput Buffering".
 * - Algorithm formal verification in Section 8.6:
 *     "Formal verification of LTTng"
 *
 * Author:
 *	Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Inspired from LTT and RelayFS:
 *  Karim Yaghmour <karim@opersys.com>
 *  Tom Zanussi <zanussi@us.ibm.com>
 *  Bob Wisniewski <bob@watson.ibm.com>
 * And from K42 :
 *  Bob Wisniewski <bob@watson.ibm.com>
 *
 * Buffer reader semantic :
 *
 * - get_subbuf_size
 * while buffer is not finalized and empty
 *   - get_subbuf
 *     - if return value != 0, continue
 *   - splice one subbuffer worth of data to a pipe
 *   - splice the data from pipe to disk/network
 *   - put_subbuf
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/idle.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/percpu.h>

#include <linux/ringbuffer/config.h>
#include <linux/ringbuffer/backend.h>
#include <linux/ringbuffer/frontend.h>
#include <linux/ringbuffer/iterator.h>

/*
 * Internal structure representing offsets to use at a sub-buffer switch.
 */
struct switch_offsets {
	unsigned long begin, end, old;
	size_t pre_header_padding, size;
	unsigned int switch_new_start:1, switch_new_end:1, switch_old_start:1,
		     switch_old_end:1;
};

DEFINE_PER_CPU(unsigned int, ring_buffer_nesting);
EXPORT_PER_CPU_SYMBOL(ring_buffer_nesting);

static
void ring_buffer_print_errors(struct channel *chan,
			      struct ring_buffer *buf,
			      int cpu);

static const struct file_operations ring_buffer_file_operations;

/*
 * Must be called under cpu hotplug protection.
 */
void ring_buffer_free(struct ring_buffer *buf)
{
	struct channel *chan = buf->backend.chan;

	ring_buffer_print_errors(chan, buf, buf->backend.cpu);
	kfree(buf->commit_hot);
	kfree(buf->commit_cold);

	ring_buffer_backend_free(&buf->backend);
}

/**
 * ring_buffer_reset - Reset ring buffer to initial values.
 * @buf: Ring buffer.
 *
 * Effectively empty the ring buffer. Should be called when the buffer is not
 * used for writing. The ring buffer can be opened for reading, but the reader
 * should not be using the iterator concurrently with reset. The previous
 * current iterator record is reset.
 */
void ring_buffer_reset(struct ring_buffer *buf)
{
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned int i;

	/*
	 * Reset iterator first. It will put the subbuffer if it currently holds
	 * it.
	 */
	ring_buffer_iterator_reset(buf);
	v_set(config, &buf->offset, 0);
	for (i = 0; i < chan->backend.num_subbuf; i++) {
		v_set(config, &buf->commit_hot[i].cc, 0);
		v_set(config, &buf->commit_hot[i].seq, 0);
		v_set(config, &buf->commit_cold[i].cc_sb, 0);
	}
	atomic_long_set(&buf->consumed, 0);
	atomic_set(&buf->record_disabled, 0);
	v_set(config, &buf->last_tsc, 0);
	ring_buffer_backend_reset(&buf->backend);
	/* Don't reset number of active readers */
	v_set(config, &buf->records_lost_full, 0);
	v_set(config, &buf->records_lost_wrap, 0);
	v_set(config, &buf->records_lost_big, 0);
	v_set(config, &buf->records_count, 0);
	v_set(config, &buf->records_overrun, 0);
	buf->finalized = 0;
}
EXPORT_SYMBOL_GPL(ring_buffer_reset);

/**
 * channel_reset - Reset channel to initial values.
 * @chan: Channel.
 *
 * Effectively empty the channel. Should be called when the channel is not used
 * for writing. The channel can be opened for reading, but the reader should not
 * be using the iterator concurrently with reset. The previous current iterator
 * record is reset.
 */
void channel_reset(struct channel *chan)
{
	/*
	 * Reset iterators first. Will put the subbuffer if held for reading.
	 */
	channel_iterator_reset(chan);
	atomic_set(&chan->record_disabled, 0);
	/* Don't reset commit_count_mask, still valid */
	channel_backend_reset(&chan->backend);
	/* Don't reset switch/read timer interval */
	/* Don't reset notifiers and notifier enable bits */
	/* Don't reset reader reference count */
}
EXPORT_SYMBOL_GPL(channel_reset);

/*
 * Must be called under cpu hotplug protection.
 */
int ring_buffer_create(struct ring_buffer *buf,
		       struct channel_backend *chanb,
		       int cpu)
{
	const struct ring_buffer_config *config = chanb->config;
	struct channel *chan = container_of(chanb, struct channel, backend);
	void *priv = chanb->priv;
	unsigned int j, num_subbuf;
	size_t subbuf_header_size;
	u64 tsc;
	int ret;

	/* Test for cpu hotplug */
	if (buf->backend.allocated)
		return 0;

	ret = ring_buffer_backend_create(&buf->backend, &chan->backend, cpu);
	if (ret)
		return ret;

	buf->commit_hot =
		kzalloc_node(ALIGN(sizeof(*buf->commit_hot)
				   * chan->backend.num_subbuf,
				   1 << INTERNODE_CACHE_SHIFT),
			GFP_KERNEL, cpu_to_node(max(cpu, 0)));
	if (!buf->commit_hot) {
		ret = -ENOMEM;
		goto free_chanbuf;
	}

	buf->commit_cold =
		kzalloc_node(ALIGN(sizeof(*buf->commit_cold)
				   * chan->backend.num_subbuf,
				   1 << INTERNODE_CACHE_SHIFT),
			GFP_KERNEL, cpu_to_node(max(cpu, 0)));
	if (!buf->commit_cold) {
		ret = -ENOMEM;
		goto free_commit;
	}

	atomic_long_set(&buf->consumed, 0);
	atomic_long_set(&buf->active_readers, 0);
	num_subbuf = chan->backend.num_subbuf;
	for (j = 0; j < num_subbuf; j++) {
		v_set(config, &buf->commit_hot[j].cc, 0);
		v_set(config, &buf->commit_hot[j].seq, 0);
		v_set(config, &buf->commit_cold[j].cc_sb, 0);
	}
	init_waitqueue_head(&buf->read_wait);
	raw_spin_lock_init(&buf->raw_idle_spinlock);

	v_set(config, &buf->records_lost_full, 0);
	v_set(config, &buf->records_lost_wrap, 0);
	v_set(config, &buf->records_lost_big, 0);
	v_set(config, &buf->records_count, 0);
	v_set(config, &buf->records_overrun, 0);
	buf->finalized = 0;

	/*
	 * Write the subbuffer header for first subbuffer so we know the total
	 * duration of data gathering.
	 */
	subbuf_header_size = config->cb.subbuffer_header_size();
	v_set(config, &buf->offset, subbuf_header_size);
	subbuffer_id_clear_noref(config, &buf->backend.buf_wsb[0].id);
	tsc = config->cb.ring_buffer_clock_read(buf->backend.chan);
	config->cb.buffer_begin(buf, tsc, 0);
	v_add(config, subbuf_header_size, &buf->commit_hot[0].cc);

	if (config->cb.buffer_create) {
		ret = config->cb.buffer_create(buf, priv, cpu, chanb->name);
		if (ret)
			goto free_init;
	}

	/*
	 * Ensure the buffer is ready before setting it to allocated and setting
	 * the cpumask.
	 * Used for cpu hotplug vs cpumask iteration.
	 */
	smp_wmb();
	buf->backend.allocated = 1;

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU) {
		CHAN_WARN_ON(chan, cpumask_test_cpu(cpu,
			     chan->backend.cpumask));
		cpumask_set_cpu(cpu, chan->backend.cpumask);
	}

	return 0;

	/* Error handling */
free_init:
	kfree(buf->commit_cold);
free_commit:
	kfree(buf->commit_hot);
free_chanbuf:
	ring_buffer_backend_free(&buf->backend);
	return ret;
}

static void switch_buffer_timer(unsigned long data)
{
	struct ring_buffer *buf = (struct ring_buffer *)data;
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;

	/*
	 * Only flush buffers periodically if readers are active.
	 */
	if (atomic_long_read(&buf->active_readers))
		ring_buffer_switch_slow(buf, SWITCH_ACTIVE);

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
		mod_timer_pinned(&buf->switch_timer,
				 jiffies + chan->switch_timer_interval);
	else
		mod_timer(&buf->switch_timer,
			  jiffies + chan->switch_timer_interval);
}

static void ring_buffer_start_switch_timer(struct ring_buffer *buf)
{
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;

	if (!chan->switch_timer_interval)
		return;

	init_timer_deferrable(&buf->switch_timer);
	buf->switch_timer.function = switch_buffer_timer;
	buf->switch_timer.expires = jiffies + chan->switch_timer_interval;
	buf->switch_timer.data = (unsigned long)buf;
	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
		add_timer_on(&buf->switch_timer, buf->backend.cpu);
	else
		add_timer(&buf->switch_timer);
}

static void ring_buffer_stop_switch_timer(struct ring_buffer *buf)
{
	struct channel *chan = buf->backend.chan;

	if (!chan->switch_timer_interval)
		return;

	del_timer_sync(&buf->switch_timer);
}

/*
 * Polling timer to check the channels for data.
 */
static void read_buffer_timer(unsigned long data)
{
	struct ring_buffer *buf = (struct ring_buffer *)data;
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;

	CHAN_WARN_ON(chan, !buf->backend.allocated);

	if (atomic_long_read(&buf->active_readers)
	    && ring_buffer_poll_deliver(config, buf, chan)) {
		wake_up_interruptible(&buf->read_wait);
		wake_up_interruptible(&chan->read_wait);
	}

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
		mod_timer_pinned(&buf->read_timer,
				 jiffies + chan->read_timer_interval);
	else
		mod_timer(&buf->read_timer,
			  jiffies + chan->read_timer_interval);
}

static void ring_buffer_start_read_timer(struct ring_buffer *buf)
{
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;

	if (config->wakeup != RING_BUFFER_WAKEUP_BY_TIMER
	    || !chan->read_timer_interval)
		return;

	init_timer_deferrable(&buf->read_timer);
	buf->read_timer.function = read_buffer_timer;
	buf->read_timer.expires = jiffies + chan->read_timer_interval;
	buf->read_timer.data = (unsigned long)buf;

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
		add_timer_on(&buf->read_timer, buf->backend.cpu);
	else
		add_timer(&buf->read_timer);
}

static void ring_buffer_stop_read_timer(struct ring_buffer *buf)
{
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;

	if (config->wakeup != RING_BUFFER_WAKEUP_BY_TIMER
	    || !chan->read_timer_interval)
		return;

	del_timer_sync(&buf->read_timer);
	/*
	 * do one more check to catch data that has been written in the last
	 * timer period.
	 */
	if (ring_buffer_poll_deliver(config, buf, chan)) {
		wake_up_interruptible(&buf->read_wait);
		wake_up_interruptible(&chan->read_wait);
	}
}

#ifdef CONFIG_HOTPLUG_CPU
/**
 *	ring_buffer_cpu_hp_callback - CPU hotplug callback
 *	@nb: notifier block
 *	@action: hotplug action to take
 *	@hcpu: CPU number
 *
 *	Returns the success/failure of the operation. (%NOTIFY_OK, %NOTIFY_BAD)
 */
static
int __cpuinit ring_buffer_cpu_hp_callback(struct notifier_block *nb,
					  unsigned long action,
					  void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	struct channel *chan = container_of(nb, struct channel,
					    cpu_hp_notifier);
	struct ring_buffer *buf = per_cpu_ptr(chan->backend.buf, cpu);
	const struct ring_buffer_config *config = chan->backend.config;

	if (!chan->cpu_hp_enable)
		return NOTIFY_DONE;

	CHAN_WARN_ON(chan, config->alloc == RING_BUFFER_ALLOC_GLOBAL);

	switch (action) {
	case CPU_DOWN_FAILED:
	case CPU_DOWN_FAILED_FROZEN:
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		ring_buffer_start_switch_timer(buf);
		ring_buffer_start_read_timer(buf);
		return NOTIFY_OK;

	case CPU_DOWN_PREPARE:
	case CPU_DOWN_PREPARE_FROZEN:
		ring_buffer_stop_switch_timer(buf);
		ring_buffer_stop_read_timer(buf);
		return NOTIFY_OK;

	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		/*
		 * Performing a buffer switch on a remote CPU. Performed by
		 * the CPU responsible for doing the hotunplug after the target
		 * CPU stopped running completely. Ensures that all data
		 * from that remote CPU is flushed.
		 */
		ring_buffer_switch_slow(buf, SWITCH_ACTIVE);
		return NOTIFY_OK;

	default:
		return NOTIFY_DONE;
	}
}
#endif


/*
 * For per-cpu buffers, call the reader wakeups before switching the buffer, so
 * that wake-up-tracing generated events are flushed before going idle. We test
 * if the spinlock is locked to deal with the race where readers try to sample
 * the ring buffer before we perform the switch. We let the readers retry in
 * that case. If there is data in the buffer, the wake up is going to forbid the
 * CPU running the reader thread from going idle.
 *
 * For a global buffer, if the client requested a reader timer, then chances are
 * we are going to keep the system from going idle anyway, so just bite the
 * bullet and do the wake up. We have no way to know if we are the last CPU
 * going to idle, so just switch the buffer. Use a spinlock to ensure we send
 * the wakeup before performing the buffer switch, in case wakeup is
 * instrumented and writes data in the buffer.
 */
static int ring_buffer_idle_callback(struct notifier_block *nb,
				  unsigned long val,
				  void *data)
{
	struct channel *chan = container_of(nb, struct channel,
					    idle_notifier);
	const struct ring_buffer_config *config = chan->backend.config;
	struct ring_buffer *buf;

	if (val != IDLE_START)
		return 0;

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
		buf = channel_get_ring_buffer(config, chan, smp_processor_id());
	else
		buf = channel_get_ring_buffer(config, chan, 0);

	raw_spin_lock(&buf->raw_idle_spinlock);
	if (config->wakeup == RING_BUFFER_WAKEUP_BY_TIMER
	    && chan->read_timer_interval
	    && atomic_long_read(&buf->active_readers)
	    && (ring_buffer_poll_deliver(config, buf, chan)
		|| ring_buffer_pending_data(config, buf, chan))) {
		wake_up_interruptible(&buf->read_wait);
		wake_up_interruptible(&chan->read_wait);
	}
	if (chan->switch_timer_interval)
		ring_buffer_switch_slow(buf, SWITCH_ACTIVE);
	raw_spin_unlock(&buf->raw_idle_spinlock);

	return 0;
}

/*
 * Holds CPU hotplug.
 */
static void channel_unregister_notifiers(struct channel *chan)
{
	const struct ring_buffer_config *config = chan->backend.config;
	int cpu;

	channel_iterator_unregister_notifiers(chan);
	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU) {
#ifdef CONFIG_HOTPLUG_CPU
		get_online_cpus();
		chan->cpu_hp_enable = 0;
		for_each_online_cpu(cpu) {
			struct ring_buffer *buf = per_cpu_ptr(chan->backend.buf,
							      cpu);
			ring_buffer_stop_switch_timer(buf);
			ring_buffer_stop_read_timer(buf);
		}
		put_online_cpus();
		unregister_cpu_notifier(&chan->cpu_hp_notifier);
#else
		for_each_possible_cpu(cpu) {
			struct ring_buffer *buf = per_cpu_ptr(chan->backend.buf,
							      cpu);
			ring_buffer_stop_switch_timer(buf);
			ring_buffer_stop_read_timer(buf);
		}
#endif
	} else {
		struct ring_buffer *buf = chan->backend.buf;

		ring_buffer_stop_switch_timer(buf);
		ring_buffer_stop_read_timer(buf);
	}
	unregister_idle_notifier(&chan->idle_notifier);
	channel_backend_unregister_notifiers(&chan->backend);
}

static void channel_free(struct channel *chan)
{
	channel_iterator_free(chan);
	channel_backend_free(&chan->backend);
	kfree(chan);
}

/**
 * channel_create - Create channel.
 * @config: ring buffer instance configuration
 * @name: name of the channel
 * @priv: ring buffer client private data
 * @buf_addr: pointer the the beginning of the preallocated buffer contiguous
 *            address mapping. It is used only by RING_BUFFER_STATIC
 *            configuration. It can be set to NULL for other backends.
 * @subbuf_size: subbuffer size
 * @num_subbuf: number of subbuffers
 * @switch_timer_interval: Time interval (in us) to fill sub-buffers with
 *                         padding to let readers get those sub-buffers.
 *                         Used for live streaming.
 * @read_timer_interval: Time interval (in us) to wake up pending readers.
 *
 * Holds cpu hotplug.
 * Returns NULL on failure.
 */
struct channel *channel_create(const struct ring_buffer_config *config,
		   const char *name, void *priv, void *buf_addr,
		   size_t subbuf_size,
		   size_t num_subbuf, unsigned int switch_timer_interval,
		   unsigned int read_timer_interval)
{
	int ret, cpu;
	struct channel *chan;

	if (ring_buffer_check_config(config,
				     switch_timer_interval,
				     read_timer_interval))
		return NULL;

	chan = kzalloc(sizeof(struct channel), GFP_KERNEL);
	if (!chan)
		return NULL;

	ret = channel_backend_init(&chan->backend, name, config, priv,
				   subbuf_size, num_subbuf);
	if (ret)
		goto error;

	ret = channel_iterator_init(chan);
	if (ret)
		goto error_free_backend;

	chan->commit_count_mask = (~0UL >> chan->backend.num_subbuf_order);
	chan->switch_timer_interval = usecs_to_jiffies(switch_timer_interval);
	chan->read_timer_interval = usecs_to_jiffies(read_timer_interval);
	init_waitqueue_head(&chan->read_wait);

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU) {
		/*
		 * In case of non-hotplug cpu, if the ring-buffer is allocated
		 * in early initcall, it will not be notified of secondary cpus.
		 * In that off case, we need to allocate for all possible cpus.
		 */
#ifdef CONFIG_HOTPLUG_CPU
		chan->cpu_hp_notifier.notifier_call =
				ring_buffer_cpu_hp_callback;
		chan->cpu_hp_notifier.priority = 6;
		register_cpu_notifier(&chan->cpu_hp_notifier);

		get_online_cpus();
		for_each_online_cpu(cpu) {
			struct ring_buffer *buf = per_cpu_ptr(chan->backend.buf,
							       cpu);
			ring_buffer_start_switch_timer(buf);
			ring_buffer_start_read_timer(buf);
		}
		chan->cpu_hp_enable = 1;
		put_online_cpus();
#else
		for_each_possible_cpu(cpu) {
			struct ring_buffer *buf = per_cpu_ptr(chan->backend.buf,
							      cpu);
			ring_buffer_start_switch_timer(buf);
			ring_buffer_start_read_timer(buf);
		}
#endif
	} else {
		struct ring_buffer *buf = chan->backend.buf;

		ring_buffer_start_switch_timer(buf);
		ring_buffer_start_read_timer(buf);
	}

	chan->idle_notifier.notifier_call = ring_buffer_idle_callback;
	/*
	 * smallest prio, run after any tracing activity, right before sleeping.
	 */
	chan->idle_notifier.priority = ~0U;
	register_idle_notifier(&chan->idle_notifier);

	return chan;

error_free_backend:
	channel_backend_free(&chan->backend);
error:
	kfree(chan);
	return NULL;
}
EXPORT_SYMBOL_GPL(channel_create);

/**
 * channel_destroy - Finalize, wait for q.s. and destroy channel.
 * @chan: channel to destroy
 *
 * Holds cpu hotplug.
 * Call "destroy" callback, finalize channels, wait for readers to release their
 * reference, then destroy ring buffer data. Note that when readers have
 * completed data consumption of finalized channels, get_subbuf() will return
 * -ENODATA. They should release their handle at that point.
 * Returns the private data pointer.
 */
void *channel_destroy(struct channel *chan)
{
	int cpu;
	const struct ring_buffer_config *config = chan->backend.config;
	void *priv;

	channel_unregister_notifiers(chan);

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU) {
		/*
		 * No need to hold cpu hotplug, because all notifiers have been
		 * unregistered.
		 */
		for_each_channel_cpu(cpu, chan) {
			struct ring_buffer *buf = per_cpu_ptr(chan->backend.buf,
							      cpu);

			if (config->cb.buffer_finalize)
				config->cb.buffer_finalize(buf,
							   chan->backend.priv,
							   cpu);
			if (buf->backend.allocated)
				ring_buffer_switch_slow(buf, SWITCH_FLUSH);
			/*
			 * Perform flush before writing to finalized.
			 */
			smp_wmb();
			ACCESS_ONCE(buf->finalized) = 1;
			wake_up_interruptible(&buf->read_wait);
		}
	} else {
		struct ring_buffer *buf = chan->backend.buf;

		if (config->cb.buffer_finalize)
			config->cb.buffer_finalize(buf, chan->backend.priv, -1);
		if (buf->backend.allocated)
			ring_buffer_switch_slow(buf, SWITCH_FLUSH);
		/*
		 * Perform flush before writing to finalized.
		 */
		smp_wmb();
		ACCESS_ONCE(buf->finalized) = 1;
		wake_up_interruptible(&buf->read_wait);
	}
	wake_up_interruptible(&chan->read_wait);

	while (atomic_long_read(&chan->read_ref) > 0)
		msleep(100);
	/* Finish waiting for refcount before free */
	smp_mb();
	priv = chan->backend.priv;
	channel_free(chan);
	return priv;
}
EXPORT_SYMBOL_GPL(channel_destroy);

struct ring_buffer *channel_get_ring_buffer(
					const struct ring_buffer_config *config,
					struct channel *chan, int cpu)
{
	if (config->alloc == RING_BUFFER_ALLOC_GLOBAL)
		return chan->backend.buf;
	else
		return per_cpu_ptr(chan->backend.buf, cpu);
}
EXPORT_SYMBOL_GPL(channel_get_ring_buffer);

int ring_buffer_open_read(struct ring_buffer *buf)
{
	struct channel *chan = buf->backend.chan;

	if (!atomic_long_add_unless(&buf->active_readers, 1, 1))
		return -EBUSY;
	atomic_long_inc(&chan->read_ref);
	smp_mb__after_atomic_inc();
	return 0;
}
EXPORT_SYMBOL_GPL(ring_buffer_open_read);

void ring_buffer_release_read(struct ring_buffer *buf)
{
	struct channel *chan = buf->backend.chan;

	CHAN_WARN_ON(chan, atomic_long_read(&buf->active_readers) != 1);
	smp_mb__before_atomic_dec();
	atomic_long_dec(&chan->read_ref);
	atomic_long_dec(&buf->active_readers);
}
EXPORT_SYMBOL_GPL(ring_buffer_release_read);

/*
 * Promote compiler barrier to a smp_mb().
 * For the specific ring buffer case, this IPI call should be removed if the
 * architecture does not reorder writes.  This should eventually be provided by
 * a separate architecture-specific infrastructure.
 */
static void remote_mb(void *info)
{
	smp_mb();
}

/**
 * ring_buffer_get_subbuf - get exclusive access to subbuffer for reading
 * @buf: ring buffer
 * @consumed: pointer where to save the consumed count value (output)
 *
 * Returns -ENODATA if buffer is finalized, -EAGAIN if there is currently no
 * data ready, or 0 if the get operation succeeds.
 * Busy-loop trying to get data if the idle sequence lock is held.
 */
int ring_buffer_get_subbuf(struct ring_buffer *buf, unsigned long *consumed)
{
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long consumed_old, consumed_idx, commit_count, write_offset;
	int ret;
	int finalized;

retry:
	finalized = ACCESS_ONCE(buf->finalized);
	/*
	 * Read finalized before counters.
	 */
	smp_rmb();
	consumed_old = atomic_long_read(&buf->consumed);
	consumed_idx = subbuf_index(consumed_old, chan);
	commit_count = v_read(config, &buf->commit_cold[consumed_idx].cc_sb);
	/*
	 * Make sure we read the commit count before reading the buffer
	 * data and the write offset. Correct consumed offset ordering
	 * wrt commit count is insured by the use of cmpxchg to update
	 * the consumed offset.
	 * smp_call_function_single can fail if the remote CPU is offline,
	 * this is OK because then there is no wmb to execute there.
	 * If our thread is executing on the same CPU as the on the buffers
	 * belongs to, we don't have to synchronize it at all. If we are
	 * migrated, the scheduler will take care of the memory barriers.
	 * Normally, smp_call_function_single() should ensure program order when
	 * executing the remote function, which implies that it surrounds the
	 * function execution with :
	 * smp_mb()
	 * send IPI
	 * csd_lock_wait
	 *                recv IPI
	 *                smp_mb()
	 *                exec. function
	 *                smp_mb()
	 *                csd unlock
	 * smp_mb()
	 *
	 * However, smp_call_function_single() does not seem to clearly execute
	 * such barriers. It depends on spinlock semantic to provide the barrier
	 * before executing the IPI and, when busy-looping, csd_lock_wait only
	 * executes smp_mb() when it has to wait for the other CPU.
	 *
	 * I don't trust this code. Therefore, let's add the smp_mb() sequence
	 * required ourself, even if duplicated. It has no performance impact
	 * anyway.
	 *
	 * smp_mb() is needed because smp_rmb() and smp_wmb() only order read vs
	 * read and write vs write. They do not ensure core synchronization. We
	 * really have to ensure total order between the 3 barriers running on
	 * the 2 CPUs.
	 */
	if (config->ipi == RING_BUFFER_IPI_BARRIER) {
		if (config->sync == RING_BUFFER_SYNC_PER_CPU
		    && config->alloc == RING_BUFFER_ALLOC_PER_CPU) {
			if (raw_smp_processor_id() != buf->backend.cpu) {
				/* Total order with IPI handler smp_mb() */
				smp_mb();
				smp_call_function_single(buf->backend.cpu,
							 remote_mb, NULL, 1);
				/* Total order with IPI handler smp_mb() */
				smp_mb();
			}
		} else {
			/* Total order with IPI handler smp_mb() */
			smp_mb();
			smp_call_function(remote_mb, NULL, 1);
			/* Total order with IPI handler smp_mb() */
			smp_mb();
		}
	} else {
		/*
		 * Local rmb to match the remote wmb to read the commit count
		 * before the buffer data and the write offset.
		 */
		smp_rmb();
	}

	write_offset = v_read(config, &buf->offset);

	/*
	 * Check that the subbuffer we are trying to consume has been
	 * already fully committed.
	 */
	if (((commit_count - chan->backend.subbuf_size)
	     & chan->commit_count_mask)
	    - (buf_trunc(consumed_old, chan)
	       >> chan->backend.num_subbuf_order)
	    != 0)
		goto nodata;

	/*
	 * Check that we are not about to read the same subbuffer in
	 * which the writer head is.
	 */
	if ((subbuf_trunc(write_offset, chan)
	   - subbuf_trunc(consumed_old, chan))
	   == 0)
		goto nodata;

	/*
	 * Failure to get the subbuffer causes a busy-loop retry without going
	 * to a wait queue. These are caused by short-lived race windows where
	 * the writer is getting access to a subbuffer we were trying to get
	 * access to. Also checks that the "consumed" buffer count we are
	 * looking for matches the one contained in the subbuffer id.
	 */
	ret = update_read_sb_index(config, &buf->backend, &chan->backend,
				   consumed_idx,
				   buf_trunc_val(consumed_old, chan));
	if (ret)
		goto retry;

	*consumed = consumed_old;
	return 0;

nodata:
	/*
	 * The memory barriers __wait_event()/wake_up_interruptible() take care
	 * of "raw_spin_is_locked" memory ordering.
	 */
	if (finalized)
		return -ENODATA;
	else if (raw_spin_is_locked(&buf->raw_idle_spinlock))
		goto retry;
	else
		return -EAGAIN;
}
EXPORT_SYMBOL_GPL(ring_buffer_get_subbuf);

/**
 * ring_buffer_put_subbuf - release exclusive subbuffer access
 * @buf: ring buffer
 * @consumed: consumed count value (output)
 */

void ring_buffer_put_subbuf(struct ring_buffer *buf, unsigned long consumed)
{
	struct ring_buffer_backend *bufb = &buf->backend;
	struct channel *chan = bufb->chan;
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long consumed_new, consumed_old, sb_bindex;

	CHAN_WARN_ON(chan, atomic_long_read(&buf->active_readers) != 1);

	consumed_old = consumed;
	consumed_new = subbuf_align(consumed_old, chan);

	/*
	 * Clear the records_unread counter. (overruns counter)
	 * Can still be non-zero if a file reader simply grabbed the data
	 * without using iterators.
	 */
	sb_bindex = subbuffer_id_get_index(config, bufb->buf_rsb.id);
	v_add(config, v_read(config, &bufb->array[sb_bindex]->records_unread),
	      &bufb->records_read);
	v_set(config, &bufb->array[sb_bindex]->records_unread, 0);
	CHAN_WARN_ON(chan, config->mode == RING_BUFFER_OVERWRITE
		     && subbuffer_id_is_noref(config, bufb->buf_rsb.id));
	subbuffer_id_set_noref(config, &bufb->buf_rsb.id);

	/*
	 * We exchange the subbuffer pages. No corruption possible even if the
	 * writer did push us, because our subbuffer pages were owned by the
	 * reader. If the consumed cmpxchg fails, this is because we have been
	 * pushed by the writer in flight recorder mode. Don't retry, just keep
	 * the new consumed offset given by the writer.
	 */
	atomic_long_cmpxchg(&buf->consumed, consumed_old, consumed_new);
}
EXPORT_SYMBOL_GPL(ring_buffer_put_subbuf);

/*
 * cons_offset is an iterator on all subbuffer offsets between the reader
 * position and the writer position. (inclusive)
 */
static
void ring_buffer_print_subbuffer_errors(struct ring_buffer *buf,
					struct channel *chan,
					unsigned long cons_offset,
					int cpu)
{
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long cons_idx, commit_count, commit_count_sb;

	cons_idx = subbuf_index(cons_offset, chan);
	commit_count = v_read(config, &buf->commit_hot[cons_idx].cc);
	commit_count_sb = v_read(config, &buf->commit_cold[cons_idx].cc_sb);

	if (subbuf_offset(commit_count, chan) != 0)
		printk(KERN_WARNING
		       "ring buffer %s, cpu %d: "
		       "commit count in subbuffer %lu,\n"
		       "expecting multiples of %lu bytes\n"
		       "  [ %lu bytes committed, %lu bytes reader-visible ]\n",
		       chan->backend.name, cpu, cons_idx,
		       chan->backend.subbuf_size,
		       commit_count, commit_count_sb);

	printk(KERN_DEBUG "ring buffer: %s, cpu %d: %lu bytes committed\n",
	       chan->backend.name, cpu, commit_count);
}

static
void ring_buffer_print_buffer_errors(struct ring_buffer *buf,
				     struct channel *chan,
				     void *priv, int cpu)
{
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long write_offset, cons_offset;

	/*
	 * Can be called in the error path of allocation when
	 * trans_channel_data is not yet set.
	 */
	if (!chan)
		return;
	/*
	 * No need to order commit_count, write_offset and cons_offset reads
	 * because we execute at teardown when no more writer nor reader
	 * references are left.
	 */
	write_offset = v_read(config, &buf->offset);
	cons_offset = atomic_long_read(&buf->consumed);
	if (write_offset != cons_offset)
		printk(KERN_WARNING
		       "ring buffer %s, cpu %d: "
		       "non-consumed data\n"
		       "  [ %lu bytes written, %lu bytes read ]\n",
		       chan->backend.name, cpu, write_offset, cons_offset);

	for (cons_offset = atomic_long_read(&buf->consumed);
	     (long) (subbuf_trunc((unsigned long) v_read(config, &buf->offset),
				  chan)
		     - cons_offset) > 0;
	     cons_offset = subbuf_align(cons_offset, chan))
		ring_buffer_print_subbuffer_errors(buf, chan, cons_offset,
						   cpu);
}

static
void ring_buffer_print_errors(struct channel *chan,
			      struct ring_buffer *buf,
			      int cpu)
{
	const struct ring_buffer_config *config = chan->backend.config;
	void *priv = chan->backend.priv;

	printk(KERN_DEBUG "ring buffer %s, cpu %d: %lu records written, "
			  "%lu records overrun\n",
			  chan->backend.name, cpu,
			  v_read(config, &buf->records_count),
			  v_read(config, &buf->records_overrun));

	if (v_read(config, &buf->records_lost_full)
	    || v_read(config, &buf->records_lost_wrap)
	    || v_read(config, &buf->records_lost_big))
		printk(KERN_WARNING
		       "ring buffer %s, cpu %d: records were lost. Caused by:\n"
		       "  [ %lu buffer full, %lu nest buffer wrap-around, "
		       "%lu event too big ]\n",
		       chan->backend.name, cpu,
		       v_read(config, &buf->records_lost_full),
		       v_read(config, &buf->records_lost_wrap),
		       v_read(config, &buf->records_lost_big));

	ring_buffer_print_buffer_errors(buf, chan, priv, cpu);
}

/*
 * ring_buffer_switch_old_start: Populate old subbuffer header.
 *
 * Only executed when the buffer is finalized, in SWITCH_FLUSH.
 */
static
void ring_buffer_switch_old_start(struct ring_buffer *buf,
				  struct channel *chan,
				  struct switch_offsets *offsets,
				  u64 tsc)
{
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long oldidx = subbuf_index(offsets->old, chan);
	unsigned long commit_count;

	config->cb.buffer_begin(buf, tsc, oldidx);

	/*
	 * Order all writes to buffer before the commit count update that will
	 * determine that the subbuffer is full.
	 */
	if (config->ipi == RING_BUFFER_IPI_BARRIER) {
		/*
		 * Must write slot data before incrementing commit count.  This
		 * compiler barrier is upgraded into a smp_mb() by the IPI sent
		 * by get_subbuf().
		 */
		barrier();
	} else
		smp_wmb();
	v_add(config, config->cb.subbuffer_header_size(),
	      &buf->commit_hot[oldidx].cc);
	commit_count = v_read(config, &buf->commit_hot[oldidx].cc);
	/* Check if the written buffer has to be delivered */
	ring_buffer_check_deliver(config, buf, chan, offsets->old, commit_count,
				  oldidx);
	ring_buffer_write_commit_counter(config, buf, chan, oldidx,
					 offsets->old, commit_count,
					 config->cb.subbuffer_header_size());
}

/*
 * ring_buffer_switch_old_end: switch old subbuffer
 *
 * Note : offset_old should never be 0 here. It is ok, because we never perform
 * buffer switch on an empty subbuffer in SWITCH_ACTIVE mode. The caller
 * increments the offset_old value when doing a SWITCH_FLUSH on an empty
 * subbuffer.
 */
static
void ring_buffer_switch_old_end(struct ring_buffer *buf,
					   struct channel *chan,
					   struct switch_offsets *offsets,
					   u64 tsc)
{
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long oldidx = subbuf_index(offsets->old - 1, chan);
	unsigned long commit_count, padding_size, data_size;

	data_size = subbuf_offset(offsets->old - 1, chan) + 1;
	padding_size = chan->backend.subbuf_size - data_size;
	subbuffer_set_data_size(config, &buf->backend, oldidx, data_size);

	/*
	 * Order all writes to buffer before the commit count update that will
	 * determine that the subbuffer is full.
	 */
	if (config->ipi == RING_BUFFER_IPI_BARRIER) {
		/*
		 * Must write slot data before incrementing commit count.  This
		 * compiler barrier is upgraded into a smp_mb() by the IPI sent
		 * by get_subbuf().
		 */
		barrier();
	} else
		smp_wmb();
	v_add(config, padding_size, &buf->commit_hot[oldidx].cc);
	commit_count = v_read(config, &buf->commit_hot[oldidx].cc);
	ring_buffer_check_deliver(config, buf, chan, offsets->old - 1,
				  commit_count, oldidx);
	ring_buffer_write_commit_counter(config, buf, chan, oldidx,
					 offsets->old, commit_count,
					 padding_size);
}

/*
 * ring_buffer_switch_new_start: Populate new subbuffer.
 *
 * This code can be executed unordered : writers may already have written to the
 * sub-buffer before this code gets executed, caution.  The commit makes sure
 * that this code is executed before the deliver of this sub-buffer.
 */
static
void ring_buffer_switch_new_start(struct ring_buffer *buf,
					   struct channel *chan,
					   struct switch_offsets *offsets,
					   u64 tsc)
{
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long beginidx = subbuf_index(offsets->begin, chan);
	unsigned long commit_count;

	config->cb.buffer_begin(buf, tsc, beginidx);

	/*
	 * Order all writes to buffer before the commit count update that will
	 * determine that the subbuffer is full.
	 */
	if (config->ipi == RING_BUFFER_IPI_BARRIER) {
		/*
		 * Must write slot data before incrementing commit count.  This
		 * compiler barrier is upgraded into a smp_mb() by the IPI sent
		 * by get_subbuf().
		 */
		barrier();
	} else
		smp_wmb();
	v_add(config, config->cb.subbuffer_header_size(),
	      &buf->commit_hot[beginidx].cc);
	commit_count = v_read(config, &buf->commit_hot[beginidx].cc);
	/* Check if the written buffer has to be delivered */
	ring_buffer_check_deliver(config, buf, chan, offsets->begin,
				  commit_count, beginidx);
	ring_buffer_write_commit_counter(config, buf, chan, beginidx,
					 offsets->begin, commit_count,
					 config->cb.subbuffer_header_size());
}

/*
 * ring_buffer_switch_new_end: finish switching current subbuffer
 *
 * The only remaining threads could be the ones with pending commits. They will
 * have to do the deliver themselves.
 */
static
void ring_buffer_switch_new_end(struct ring_buffer *buf,
					    struct channel *chan,
					    struct switch_offsets *offsets,
					    u64 tsc)
{
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long endidx = subbuf_index(offsets->end - 1, chan);
	unsigned long commit_count, padding_size, data_size;

	data_size = subbuf_offset(offsets->end - 1, chan) + 1;
	padding_size = chan->backend.subbuf_size - data_size;
	subbuffer_set_data_size(config, &buf->backend, endidx, data_size);

	/*
	 * Order all writes to buffer before the commit count update that will
	 * determine that the subbuffer is full.
	 */
	if (config->ipi == RING_BUFFER_IPI_BARRIER) {
		/*
		 * Must write slot data before incrementing commit count.  This
		 * compiler barrier is upgraded into a smp_mb() by the IPI sent
		 * by get_subbuf().
		 */
		barrier();
	} else
		smp_wmb();
	v_add(config, padding_size, &buf->commit_hot[endidx].cc);
	commit_count = v_read(config, &buf->commit_hot[endidx].cc);
	ring_buffer_check_deliver(config, buf, chan, offsets->end - 1,
				  commit_count, endidx);
	ring_buffer_write_commit_counter(config, buf, chan, endidx,
					 offsets->end, commit_count,
					 padding_size);
}

/*
 * Returns :
 * 0 if ok
 * !0 if execution must be aborted.
 */
static
int ring_buffer_try_switch_slow(enum switch_mode mode,
				struct ring_buffer *buf,
				struct channel *chan,
				struct switch_offsets *offsets,
				u64 *tsc)
{
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long off;

	offsets->begin = v_read(config, &buf->offset);
	offsets->old = offsets->begin;
	offsets->switch_old_start = 0;
	off = subbuf_offset(offsets->begin, chan);

	*tsc = config->cb.ring_buffer_clock_read(chan);

	/*
	 * Ensure we flush the header of an empty subbuffer when doing the
	 * finalize (SWITCH_FLUSH). This ensures that we end up knowing the
	 * total data gathering duration even if there were no records saved
	 * after the last buffer switch.
	 * In SWITCH_ACTIVE mode, switch the buffer when it contains events.
	 * SWITCH_ACTIVE only flushes the current subbuffer, dealing with end of
	 * subbuffer header as appropriate.
	 * The next record that reserves space will be responsible for
	 * populating the following subbuffer header. We choose not to populate
	 * the next subbuffer header here because we want to be able to use
	 * SWITCH_ACTIVE for periodical buffer flush and CPU idle entry buffer
	 * flush, which must guarantee that all the buffer content (records and
	 * header timestamps) are visible to the reader. This is required for
	 * quiescence guarantees for the fusion merge.
	 */
	if (mode == SWITCH_FLUSH || off > 0) {
		if (unlikely(off == 0)) {
			/*
			 * The client does not save any header information.
			 * Don't switch empty subbuffer on finalize, because it
			 * is invalid to deliver a completely empty subbuffer.
			 */
			if (!config->cb.subbuffer_header_size())
				return -1;
			/*
			 * Need to write the subbuffer start header on finalize.
			 */
			offsets->switch_old_start = 1;
		}
		offsets->begin = subbuf_align(offsets->begin, chan);
	} else
		return -1;	/* we do not have to switch : buffer is empty */
	/* Note: old points to the next subbuf at offset 0 */
	offsets->end = offsets->begin;
	return 0;
}

/*
 * Force a sub-buffer switch. This operation is completely reentrant : can be
 * called while tracing is active with absolutely no lock held.
 *
 * Note, however, that as a v_cmpxchg is used for some atomic
 * operations, this function must be called from the CPU which owns the buffer
 * for a ACTIVE flush.
 */
void ring_buffer_switch_slow(struct ring_buffer *buf, enum switch_mode mode)
{
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;
	struct switch_offsets offsets;
	unsigned long oldidx;
	u64 tsc;

	offsets.size = 0;

	/*
	 * Perform retryable operations.
	 */
	do {
		if (ring_buffer_try_switch_slow(mode, buf, chan, &offsets,
						&tsc))
			return;	/* Switch not needed */
	} while (v_cmpxchg(config, &buf->offset, offsets.old, offsets.end)
		 != offsets.old);

	/*
	 * Atomically update last_tsc. This update races against concurrent
	 * atomic updates, but the race will always cause supplementary full TSC
	 * records, never the opposite (missing a full TSC record when it would
	 * be needed).
	 */
	save_last_tsc(config, buf, tsc);

	/*
	 * Push the reader if necessary
	 */
	ring_buffer_reserve_push_reader(buf, chan, offsets.old);

	oldidx = subbuf_index(offsets.old, chan);
	ring_buffer_clear_noref(config, &buf->backend, oldidx);

	/*
	 * May need to populate header start on SWITCH_FLUSH.
	 */
	if (offsets.switch_old_start) {
		ring_buffer_switch_old_start(buf, chan, &offsets, tsc);
		offsets.old += config->cb.subbuffer_header_size();
	}

	/*
	 * Switch old subbuffer.
	 */
	ring_buffer_switch_old_end(buf, chan, &offsets, tsc);
}
EXPORT_SYMBOL_GPL(ring_buffer_switch_slow);

/*
 * Returns :
 * 0 if ok
 * !0 if execution must be aborted.
 */
static
int ring_buffer_try_reserve_slow(struct ring_buffer *buf, struct channel *chan,
				 struct switch_offsets *offsets,
				 struct ring_buffer_ctx *ctx)
{
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long reserve_commit_diff;

	offsets->begin = v_read(config, &buf->offset);
	offsets->old = offsets->begin;
	offsets->switch_new_start = 0;
	offsets->switch_new_end = 0;
	offsets->switch_old_end = 0;
	offsets->pre_header_padding = 0;

	ctx->tsc = config->cb.ring_buffer_clock_read(chan);

	if (last_tsc_overflow(config, buf, ctx->tsc))
		ctx->rflags = RING_BUFFER_RFLAG_FULL_TSC;

	if (unlikely(subbuf_offset(offsets->begin, ctx->chan) == 0)) {
		offsets->switch_new_start = 1;		/* For offsets->begin */
	} else {
		offsets->size = config->cb.record_header_size(config, chan,
						offsets->begin,
						ctx->data_size,
						&offsets->pre_header_padding,
						ctx->rflags, ctx);
		offsets->size +=
			ring_buffer_align(config,
					  offsets->begin + offsets->size,
					  ctx->largest_align)
			+ ctx->data_size;
		if (unlikely((subbuf_offset(offsets->begin, chan) +
			     offsets->size) > chan->backend.subbuf_size)) {
			offsets->switch_old_end = 1;	/* For offsets->old */
			offsets->switch_new_start = 1;	/* For offsets->begin */
		}
	}
	if (unlikely(offsets->switch_new_start)) {
		unsigned long sb_index;

		/*
		 * We are typically not filling the previous buffer completely.
		 */
		if (likely(offsets->switch_old_end))
			offsets->begin = subbuf_align(offsets->begin, chan);
		offsets->begin = offsets->begin
				 + config->cb.subbuffer_header_size();
		/* Test new buffer integrity */
		sb_index = subbuf_index(offsets->begin, chan);
		reserve_commit_diff =
		  (buf_trunc(offsets->begin, chan)
		   >> chan->backend.num_subbuf_order)
		  - ((unsigned long) v_read(config,
					    &buf->commit_cold[sb_index].cc_sb)
		     & chan->commit_count_mask);
		if (likely(reserve_commit_diff == 0)) {
			/* Next subbuffer not being written to. */
			if (unlikely(config->mode != RING_BUFFER_OVERWRITE &&
				(subbuf_trunc(offsets->begin, chan)
				 - subbuf_trunc((unsigned long)
				     atomic_long_read(&buf->consumed), chan))
				>= chan->backend.buf_size)) {
				/*
				 * We do not overwrite non consumed buffers
				 * and we are full : record is lost.
				 */
				v_inc(config, &buf->records_lost_full);
				return -1;
			} else {
				/*
				 * Next subbuffer not being written to, and we
				 * are either in overwrite mode or the buffer is
				 * not full. It's safe to write in this new
				 * subbuffer.
				 */
			}
		} else {
			/*
			 * Next subbuffer reserve offset does not match the
			 * commit offset. Drop record in producer-consumer and
			 * overwrite mode. Caused by either a writer OOPS or too
			 * many nested writes over a reserve/commit pair.
			 */
			v_inc(config, &buf->records_lost_wrap);
			return -1;
		}
		offsets->size =
			config->cb.record_header_size(config, chan,
						offsets->begin,
						ctx->data_size,
						&offsets->pre_header_padding,
						ctx->rflags, ctx);
		offsets->size +=
			ring_buffer_align(config,
					  offsets->begin + offsets->size,
					  ctx->largest_align)
			+ ctx->data_size;
		if (unlikely((subbuf_offset(offsets->begin, chan)
			     + offsets->size) > chan->backend.subbuf_size)) {
			/*
			 * Record too big for subbuffers, report error, don't
			 * complete the sub-buffer switch.
			 */
			v_inc(config, &buf->records_lost_big);
			return -1;
		} else {
			/*
			 * We just made a successful buffer switch and the
			 * record fits in the new subbuffer. Let's write.
			 */
		}
	} else {
		/*
		 * Record fits in the current buffer and we are not on a switch
		 * boundary. It's safe to write.
		 */
	}
	offsets->end = offsets->begin + offsets->size;

	if (unlikely((subbuf_offset(offsets->end, chan)) == 0)) {
		/*
		 * The offset_end will fall at the very beginning of the next
		 * subbuffer.
		 */
		offsets->switch_new_end = 1;	/* For offsets->begin */
	}
	return 0;
}

/**
 * ring_buffer_reserve_slow - Atomic slot reservation in a buffer.
 * @ctx: ring buffer context.
 *
 * Return : -ENOSPC if not enough space, else returns 0.
 * It will take care of sub-buffer switching.
 */
int ring_buffer_reserve_slow(struct ring_buffer_ctx *ctx)
{
	struct channel *chan = ctx->chan;
	const struct ring_buffer_config *config = chan->backend.config;
	struct ring_buffer *buf;
	struct switch_offsets offsets;

	if (config->alloc == RING_BUFFER_ALLOC_PER_CPU)
		buf = per_cpu_ptr(chan->backend.buf, ctx->cpu);
	else
		buf = chan->backend.buf;
	ctx->buf = buf;

	offsets.size = 0;

	do {
		if (unlikely(ring_buffer_try_reserve_slow(buf, chan, &offsets,
							  ctx)))
			return -ENOSPC;
	} while (unlikely(v_cmpxchg(config, &buf->offset, offsets.old,
				    offsets.end)
			  != offsets.old));

	/*
	 * Atomically update last_tsc. This update races against concurrent
	 * atomic updates, but the race will always cause supplementary full TSC
	 * records, never the opposite (missing a full TSC record when it would
	 * be needed).
	 */
	save_last_tsc(config, buf, ctx->tsc);

	/*
	 * Push the reader if necessary
	 */
	ring_buffer_reserve_push_reader(buf, chan, offsets.end - 1);

	/*
	 * Clear noref flag for this subbuffer.
	 */
	ring_buffer_clear_noref(config, &buf->backend,
				subbuf_index(offsets.end - 1, chan));

	/*
	 * Switch old subbuffer if needed.
	 */
	if (unlikely(offsets.switch_old_end)) {
		ring_buffer_clear_noref(config, &buf->backend,
					subbuf_index(offsets.old - 1, chan));
		ring_buffer_switch_old_end(buf, chan, &offsets, ctx->tsc);
	}

	/*
	 * Populate new subbuffer.
	 */
	if (unlikely(offsets.switch_new_start))
		ring_buffer_switch_new_start(buf, chan, &offsets, ctx->tsc);

	if (unlikely(offsets.switch_new_end))
		ring_buffer_switch_new_end(buf, chan, &offsets, ctx->tsc);

	ctx->slot_size = offsets.size;
	ctx->pre_offset = offsets.begin;
	ctx->buf_offset = offsets.begin + offsets.pre_header_padding;
	return 0;
}
EXPORT_SYMBOL_GPL(ring_buffer_reserve_slow);
