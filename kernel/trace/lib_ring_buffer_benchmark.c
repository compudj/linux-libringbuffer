/*
 * ring buffer library tester and benchmark
 *
 * Copyright (C) 2009 Steven Rostedt <srostedt@redhat.com>
 * Copyright (C) 2010 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#define RING_BUFFER_ALIGN

#include <linux/ringbuffer/config.h>
#include <linux/trace_clock.h>
#include <linux/completion.h>
#include <linux/kmemcheck.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/time.h>
#include <asm/local.h>

struct subbuffer_header {
	u64		ts;
	unsigned long	commit;
	uint8_t		header_end[0];
};

/*
 * Only 27-bit tsc support, TODO: extended header to support 27-bit overflow.
 */
struct event_header {
	kmemcheck_bitfield_begin(bitfield);
	u32		type_len:5, tsc:27;
	kmemcheck_bitfield_end(bitfield);
	uint8_t		header_end[0];
};

struct payload {
	int cpuid;
} RING_BUFFER_ALIGN_ATTR;

static inline notrace u64 ring_buffer_clock_read(struct channel *chan)
{
	return trace_clock_local();
}

static inline notrace
unsigned char record_header_size(const struct ring_buffer_config *config,
				 struct channel *chan, size_t offset,
				 size_t data_size, size_t *pre_header_padding,
				 unsigned int rflags,
				 struct ring_buffer_ctx *ctx)
{
	size_t orig_offset = offset;
	size_t padding;

	padding = ring_buffer_align(config, offset,
				    offsetof(struct event_header, header_end));
	offset += padding;
	offset += sizeof(struct event_header);

	*pre_header_padding = padding;
	return offset - orig_offset;
}

#include <linux/ringbuffer/api.h>

static struct channel *channel;
static const struct ring_buffer_config client_config;

static u64 client_ring_buffer_clock_read(struct channel *chan)
{
	return ring_buffer_clock_read(chan);
}

static
size_t client_record_header_size(const struct ring_buffer_config *config,
				struct channel *chan, size_t offset,
				size_t data_size,
				size_t *pre_header_padding,
				unsigned int rflags,
				struct ring_buffer_ctx *ctx)
{
	return record_header_size(config, chan, offset, data_size,
				  pre_header_padding, rflags, ctx);
}

static size_t client_subbuffer_header_size(void)
{
	return offsetof(struct subbuffer_header, header_end);
}

static void client_buffer_begin(struct ring_buffer *buf, u64 tsc,
				unsigned int subbuf_idx)
{
	struct channel *chan = buf->backend.chan;
	struct subbuffer_header *header =
		(struct subbuffer_header *)
			ring_buffer_offset_address(&buf->backend,
				subbuf_idx * chan->backend.subbuf_size);
	header->ts = tsc;
}

static void client_buffer_end(struct ring_buffer *buf, u64 tsc,
			    unsigned int subbuf_idx, unsigned long data_size)
{
	struct channel *chan = buf->backend.chan;
	struct subbuffer_header *header =
		(struct subbuffer_header *)
			ring_buffer_offset_address(&buf->backend,
				subbuf_idx * chan->backend.subbuf_size);
	header->commit = data_size;
}

static const struct ring_buffer_config client_config = {
	.cb.ring_buffer_clock_read = client_ring_buffer_clock_read,
	.cb.record_header_size = client_record_header_size,
	.cb.subbuffer_header_size = client_subbuffer_header_size,
	.cb.buffer_begin = client_buffer_begin,
	.cb.buffer_end = client_buffer_end,
	.cb.buffer_create = NULL,
	.cb.buffer_finalize = NULL,

	.tsc_bits = 27,
	.alloc = RING_BUFFER_ALLOC_PER_CPU,
	/* .alloc = RING_BUFFER_ALLOC_GLOBAL, */
	.sync = RING_BUFFER_SYNC_PER_CPU,
	/* .sync = RING_BUFFER_SYNC_GLOBAL, */
	.mode = RING_BUFFER_OVERWRITE,
	/* .mode = RING_BUFFER_DISCARD, */
	.align = RING_BUFFER_NATURAL,
	.backend = RING_BUFFER_PAGE,
	.output = RING_BUFFER_NONE,
	.oops = RING_BUFFER_NO_OOPS_CONSISTENCY,
	.ipi = RING_BUFFER_IPI_BARRIER,
	.wakeup = RING_BUFFER_WAKEUP_BY_TIMER,
};

static void write_event_header(const struct ring_buffer_config *config,
			       struct ring_buffer_ctx *ctx)
{
	struct event_header header;

	header.type_len = ctx->data_size;
	header.tsc = ctx->tsc;
	/*
	 * eventually check rflags to know if a 27 bit tsc overflow is detected
	 * between consecutive events.
	 */
	ring_buffer_write(config, ctx, &header, sizeof(header));
}

/* run time and sleep time in seconds */
#define RUN_TIME	10
#define SLEEP_TIME	10

#ifndef CONFIG_PREEMPT
/* number of events for writer to give up the cpu */
static int resched_interval = 5000;
#endif

static struct completion read_start;

static struct task_struct *producer;
static struct task_struct *consumer;
static unsigned long iter_read;
static unsigned long long global_read;

static int disable_reader;
module_param(disable_reader, uint, 0644);
MODULE_PARM_DESC(disable_reader, "only run producer");

static int write_iteration = 50;
module_param(write_iteration, uint, 0644);
MODULE_PARM_DESC(write_iteration, "# of writes between timestamp readings");

static int producer_nice = 19;
static int consumer_nice = 19;

static int producer_fifo = -1;
static int consumer_fifo = -1;

module_param(producer_nice, uint, 0644);
MODULE_PARM_DESC(producer_nice, "nice prio for producer");

module_param(consumer_nice, uint, 0644);
MODULE_PARM_DESC(consumer_nice, "nice prio for consumer");

module_param(producer_fifo, uint, 0644);
MODULE_PARM_DESC(producer_fifo, "fifo prio for producer");

module_param(consumer_fifo, uint, 0644);
MODULE_PARM_DESC(consumer_fifo, "fifo prio for consumer");

static int kill_test;

#define KILL_TEST()				\
	do {					\
		if (!kill_test) {		\
			kill_test = 1;		\
			WARN_ON(1);		\
		}				\
	} while (0)

enum event_status {
	EVENT_FOUND,
	EVENT_DROPPED,
};

static int read_subbuffer(struct ring_buffer *buf, int cpu)
{
	unsigned long consumed, offset, data_size;
	int ret;

	ret = ring_buffer_get_subbuf(buf, &consumed);
	if (ret && !ACCESS_ONCE(buf->finalized)
	    && client_config.alloc == RING_BUFFER_ALLOC_GLOBAL) {
		/*
		 * Use "pull" scheme for global buffers. The reader
		 * itself flushes the buffer to "pull" data not visible
		 * to readers yet. Flush current subbuffer and re-try.
		 *
		 * Per-CPU buffers rather use a "push" scheme because
		 * the IPI needed to flush all CPU's buffers is too
		 * costly. In the "push" scheme, the reader waits for
		 * the writer periodic deferrable timer to flush the
		 * buffers (keeping track of a quiescent state
		 * timestamp). Therefore, the writer "pushes" data out
		 * of the buffers rather than letting the reader "pull"
		 * data from the buffer.
		 */
		ring_buffer_switch(&client_config, buf, SWITCH_ACTIVE);
		ret = ring_buffer_get_subbuf(buf, &consumed);
	}
	if (ret)
		goto get_fail;

	data_size = ring_buffer_get_read_data_size(&client_config, buf);
	offset = consumed;

	/* Skip header */
	offset += client_subbuffer_header_size();

	/* Read events */
	while (offset < consumed + data_size) {
		unsigned int cpuid;

		offset += ring_buffer_align(&client_config, offset,
					    offsetof(struct event_header,
						     header_end));
		offset += offsetof(struct event_header, header_end);
		offset += ring_buffer_align(&client_config, offset,
					    sizeof(cpuid));
		ring_buffer_read(&buf->backend, offset, &cpuid, sizeof(cpuid));
		if (client_config.alloc == RING_BUFFER_ALLOC_GLOBAL)
			WARN_ON_ONCE(cpuid > num_online_cpus());
		else
			WARN_ON_ONCE(cpuid != cpu);
		offset += sizeof(cpuid);
		iter_read++;
		global_read++;
	}

	/* Put subbuffer */
	ring_buffer_put_subbuf(buf, consumed);
get_fail:

	return ret;
}

static int read_channel_events(void)
{
	int cpu;
	int all_finalized = 1;
	int all_empty = 1;

	for_each_channel_cpu(cpu, channel) {
		struct ring_buffer *buf;
		int ret;

		buf = channel_get_ring_buffer(&client_config, channel, cpu);
		ret = read_subbuffer(buf, cpu);
		if (ret != -ENODATA)
			all_finalized = 0;
		if (ret == 0)
			all_empty = 0;
	}

	if (all_finalized)
		return -ENODATA;
	if (all_empty)
		return -EAGAIN;
	else
		return 0;
}

static void ring_buffer_consumer(void)
{
	int ret;

	do {
		if (client_config.alloc == RING_BUFFER_ALLOC_GLOBAL) {
			struct ring_buffer *buf;

			buf = channel_get_ring_buffer(
				&client_config, channel, 0);
			ret = read_subbuffer(buf, 0);
			if (ret == -EAGAIN)
				wait_event_interruptible(channel->read_wait,
					(ret = read_subbuffer(buf, 0),
					 ret != -EAGAIN));
		} else {
			ret = read_channel_events();
			if (ret == -EAGAIN)
				wait_event_interruptible(channel->read_wait,
					(ret = read_channel_events(),
					 ret != -EAGAIN));
		}
	} while (!kill_test && ret != -ENODATA);
}

static void ring_buffer_producer(void)
{
	struct timeval start_tv;
	struct timeval end_tv;
	unsigned long long time;

	unsigned long long hit = 0;
	unsigned long long missed = 0;

	unsigned long long written = 0;
	unsigned long long lost_full = 0, lost_wrap = 0, lost_big = 0;
	unsigned long long entries = 0;
	unsigned long long overruns = 0;
	unsigned long long read = 0;
	unsigned long avg;
	int cnt = 0;
	int ret, cpu;
	struct ring_buffer_ctx ctx;

	/*
	 * Hammer the buffer for 10 secs (this may
	 * make the system stall)
	 */
	trace_printk("Starting ring buffer hammer\n");
	do_gettimeofday(&start_tv);
	do {
		int i;

		for (i = 0; i < write_iteration; i++) {
			cpu = ring_buffer_get_cpu(&client_config);
			if (cpu < 0)
				continue;
			ring_buffer_ctx_init(&ctx, channel, NULL,
					     sizeof(struct payload),
					     sizeof(cpuid), cpu);
			ret = ring_buffer_reserve(&client_config, &ctx);
			if (ret) {
				missed++;
			} else {
				int cpuid = smp_processor_id();

				hit++;
				write_event_header(&client_config, &ctx);
				ring_buffer_align_ctx(&client_config, &ctx,
						      sizeof(cpuid));
				ring_buffer_write(&client_config, &ctx,
						  &cpuid, sizeof(cpuid));
				ring_buffer_commit(&client_config, &ctx);
			}
			ring_buffer_put_cpu(&client_config);
		}
		do_gettimeofday(&end_tv);

		cnt++;

#ifndef CONFIG_PREEMPT
		/*
		 * If we are a non preempt kernel, the 10 second run will
		 * stop everything while it runs. Instead, we will call
		 * cond_resched and also add any time that was lost by a
		 * rescedule.
		 *
		 * Do a cond resched at the same frequency we would wake up
		 * the reader.
		 */
		if (cnt % resched_interval)
			cond_resched();
#endif

	} while (end_tv.tv_sec < (start_tv.tv_sec + RUN_TIME) && !kill_test);
	trace_printk("End ring buffer hammer\n");

	time = end_tv.tv_sec - start_tv.tv_sec;
	time *= USEC_PER_SEC;
	time += (long long)((long)end_tv.tv_usec - (long)start_tv.tv_usec);

	if (client_config.alloc == RING_BUFFER_ALLOC_GLOBAL) {
		struct ring_buffer *buf =
			channel_get_ring_buffer(&client_config,
						channel, 0);

		/*
		 * These values only take into account flushed subbuffers.
		 */
		written += ring_buffer_get_records_count(&client_config, buf);
		lost_full += ring_buffer_get_records_lost_full(&client_config, buf);
		lost_wrap += ring_buffer_get_records_lost_wrap(&client_config, buf);
		lost_big += ring_buffer_get_records_lost_big(&client_config, buf);
		overruns += ring_buffer_get_records_overrun(&client_config, buf);
		entries += ring_buffer_get_records_unread(&client_config, buf);
		read += ring_buffer_get_records_read(&client_config, buf);
	} else {
		for_each_channel_cpu(cpu, channel) {
			struct ring_buffer *buf =
				channel_get_ring_buffer(&client_config,
							channel, cpu);

			/*
			 * These values only take into account flushed
			 * subbuffers.
			 */
			written += ring_buffer_get_records_count(&client_config, buf);
			lost_full += ring_buffer_get_records_lost_full(&client_config, buf);
			lost_wrap += ring_buffer_get_records_lost_wrap(&client_config, buf);
			lost_big += ring_buffer_get_records_lost_big(&client_config, buf);
			overruns += ring_buffer_get_records_overrun(&client_config, buf);
			entries += ring_buffer_get_records_unread(&client_config, buf);
			read += ring_buffer_get_records_read(&client_config, buf);
		}
	}

	if (kill_test)
		trace_printk("ERROR!\n");

	if (!disable_reader) {
		if (consumer_fifo < 0)
			trace_printk("Running Consumer at nice: %d\n",
				     consumer_nice);
		else
			trace_printk("Running Consumer at SCHED_FIFO %d\n",
				     consumer_fifo);
	}
	if (producer_fifo < 0)
		trace_printk("Running Producer at nice: %d\n",
			     producer_nice);
	else
		trace_printk("Running Producer at SCHED_FIFO %d\n",
			     producer_fifo);

	/* Let the user know that the test is running at low priority */
	if (producer_fifo < 0 && consumer_fifo < 0 &&
	    producer_nice == 19 && consumer_nice == 19)
		trace_printk("WARNING!!! This test is running at lowest priority.\n");

	trace_printk("This iteration:           %llu (usecs)\n", time);
	trace_printk("  Time:                   %llu (usecs)\n", time);
	trace_printk("  Data production:\n");
	trace_printk("    Written:              %llu\n", hit);
	trace_printk("    Lost:                 %llu\n", missed);
	trace_printk("  Data consumption:\n");
	if (disable_reader)
		trace_printk("    Read:                 (reader disabled)\n");
	else
		trace_printk("    Read:                 %lu\n",
			     iter_read);
	trace_printk("\n");
	trace_printk("Global (only flushed subbuffers):\n");
	trace_printk("  Data production:\n");
	trace_printk("    Written:              %llu\n", written);
	trace_printk("    Lost (buffer full)    %llu\n", lost_full);
	trace_printk("    Lost (wrap around)    %llu\n", lost_wrap);
	trace_printk("    Lost (event too big)  %llu\n", lost_big);
	trace_printk("  Data consumption:\n");
	if (disable_reader)
		trace_printk("    Read:                 (reader disabled)\n");
	else
		trace_printk("    Read:                 %llu (%llu read-side)\n",
			     read, global_read);
	trace_printk("    Overruns:             %llu\n", overruns);
	trace_printk("    Non-consumed entries: %llu\n", entries);
	trace_printk("    Consumption total:    %llu\n",
		     entries + overruns + read);

	/* Convert time from usecs to millisecs */
	do_div(time, USEC_PER_MSEC);
	if (time)
		hit /= (long)time;
	else
		trace_printk("TIME IS ZERO??\n");

	trace_printk("Entries per millisec: %llu\n", hit);

	if (hit) {
		/* Calculate the average time in nanosecs */
		avg = NSEC_PER_MSEC / hit;
		trace_printk("%lu ns per entry written\n", avg);
	}

	if (missed) {
		if (time)
			missed /= (long)time;

		trace_printk("Total iterations per millisec: %llu\n",
			     hit + missed);

		/* it is possible that hit + missed will overflow and be zero */
		if (!(hit + missed)) {
			trace_printk("hit + missed overflowed and totalled zero!\n");
			hit--; /* make it non zero */
		}

		/* Caculate the average time in nanosecs */
		avg = NSEC_PER_MSEC / (hit + missed);
		trace_printk("%lu ns per entry (written+lost)\n", avg);
	}
	iter_read = 0;
}

static void wait_to_die(void)
{
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
}

static int ring_buffer_consumer_thread(void *arg)
{
	struct ring_buffer *buf;
	int cpu;

	wait_for_completion_interruptible(&read_start);

	if (client_config.alloc == RING_BUFFER_ALLOC_GLOBAL) {
		buf = channel_get_ring_buffer(&client_config, channel, 0);
		WARN_ON_ONCE(ring_buffer_open_read(buf));
	} else {
		/* TODO: benchmark does not take cpu hotplug into account. */
		for_each_channel_cpu(cpu, channel) {
			buf = channel_get_ring_buffer(&client_config, channel,
						      cpu);
			WARN_ON_ONCE(ring_buffer_open_read(buf));
		}
	}

	ring_buffer_consumer();

	if (client_config.alloc == RING_BUFFER_ALLOC_GLOBAL)
		ring_buffer_release_read(buf);
	else {
		/* TODO: benchmark does not take cpu hotplug into account. */
		for_each_channel_cpu(cpu, channel) {
			buf = channel_get_ring_buffer(&client_config, channel,
						      cpu);
			ring_buffer_release_read(buf);
		}
	}

	wait_to_die();

	return 0;
}

static int ring_buffer_producer_thread(void *arg)
{
	if (consumer)
		complete(&read_start);

	while (!kthread_should_stop() && !kill_test) {
		ring_buffer_producer();

		trace_printk("Sleeping for 10 secs\n");
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ * SLEEP_TIME);
		__set_current_state(TASK_RUNNING);
	}

	if (kill_test)
		wait_to_die();

	return 0;
}

static int __init ring_buffer_benchmark_init(void)
{
	int ret;

	/* make a one meg buffer in overwite mode */
	/* altern. ftrace equivalent: 4k subbuffers: 4096 * 256. */
	channel = channel_create(&client_config, "benchmark", NULL, NULL,
				 /* 4096, 256, */
				 131072, 8,
				 /* 524288, 2, */
				 100000, 100000);
	if (!channel)
		return -EINVAL;

	if (!disable_reader) {
		init_completion(&read_start);
		consumer = kthread_run(ring_buffer_consumer_thread,
				       NULL, "rb_consumer");
		ret = PTR_ERR(consumer);
		if (IS_ERR(consumer))
			goto out_fail;
	}

	producer = kthread_run(ring_buffer_producer_thread,
			       NULL, "rb_producer");
	ret = PTR_ERR(producer);

	if (IS_ERR(producer))
		goto out_kill;

	/*
	 * Run them as low-prio background tasks by default:
	 */
	if (!disable_reader) {
		if (consumer_fifo >= 0) {
			struct sched_param param = {
				.sched_priority = consumer_fifo
			};
			sched_setscheduler(consumer, SCHED_FIFO, &param);
		} else
			set_user_nice(consumer, consumer_nice);
	}

	if (producer_fifo >= 0) {
		struct sched_param param = {
			.sched_priority = consumer_fifo
		};
		sched_setscheduler(producer, SCHED_FIFO, &param);
	} else
		set_user_nice(producer, producer_nice);

	return 0;

out_kill:
	if (consumer)
		kthread_kill_stop(consumer, SIGKILL);
out_fail:
	channel_destroy(channel);
	return ret;
}

static void __exit ring_buffer_benchmark_exit(void)
{
	kthread_kill_stop(producer, SIGKILL);
	channel_destroy(channel);
	if (consumer)
		kthread_kill_stop(consumer, SIGKILL);
}

module_init(ring_buffer_benchmark_init);
module_exit(ring_buffer_benchmark_exit);

MODULE_AUTHOR("Mathieu Desnoyers");
MODULE_DESCRIPTION("lib_ring_buffer_benchmark");
MODULE_LICENSE("GPL");
