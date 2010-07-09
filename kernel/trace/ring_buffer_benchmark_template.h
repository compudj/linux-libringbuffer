/*
 * ring buffer tester and benchmark template
 *
 * This template file is meant to be included in respective tester variant
 * modules with appropriate definitions.
 *
 * Copyright (C) 2009 Steven Rostedt <srostedt@redhat.com>
 * Copyright (C) 2010 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/trace_clock.h>
#include <linux/completion.h>
#include <linux/kmemcheck.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/debugfs.h>
#include <linux/stringify.h>
#include <asm/local.h>

#include <linux/ringbuffer/config.h>		/* only for counter access */
#include <linux/ringbuffer/backend.h>		/* only for counter access */
#include <linux/ringbuffer/frontend.h>		/* only for counter access */

#define BUFFER_SIZE	1048576			/* Use 1MB buffers */

#ifdef RING_BUFFER_GLOBAL_TMPL
# ifdef RING_BUFFER_OVERWRITE_TMPL
#  include <linux/ringbuffer/global_overwrite.h>
#  define RING_BUFFER_TMPL(x)	ring_buffer_global_overwrite_##x
# else
#  include <linux/ringbuffer/global_discard.h>
#  define RING_BUFFER_TMPL(x)	ring_buffer_global_discard_##x
# endif
#elif defined(RING_BUFFER_PER_CPU_TMPL)
# ifdef RING_BUFFER_OVERWRITE_TMPL
#  include <linux/ringbuffer/percpu_overwrite.h>
#  define RING_BUFFER_TMPL(x)	ring_buffer_percpu_overwrite_##x
# else
#  include <linux/ringbuffer/percpu_discard.h>
#  define RING_BUFFER_TMPL(x)	ring_buffer_percpu_discard_##x
# endif
#elif defined(RING_BUFFER_PER_CPU_LOCAL_TMPL)
# ifdef RING_BUFFER_OVERWRITE_TMPL
#  include <linux/ringbuffer/percpu_local_overwrite.h>
#  define RING_BUFFER_TMPL(x)	ring_buffer_percpu_local_overwrite_##x
# else
#  include <linux/ringbuffer/percpu_local_discard.h>
#  define RING_BUFFER_TMPL(x)	ring_buffer_percpu_local_discard_##x
# endif
#else
#error "Please define one type of ring buffer template"
#endif

#if (!defined(RING_BUFFER_OVERWRITE_TMPL) \
	&& !defined(RING_BUFFER_DISCARD_TMPL))
#error "Please define one mode of ring buffer template"
#endif

#ifndef RING_BUFFER_NAME_TMPL
#error "Please define the ring buffer template name"
#endif

static struct channel *channel;

#ifdef RING_BUFFER_PER_CPU_LOCAL_TMPL
static struct dentry *dentry[NR_CPUS];
#else
static struct dentry *dentry;
#endif

/* run time and sleep time in seconds */
#define RUN_TIME	10
#define SLEEP_TIME	10

#ifndef CONFIG_PREEMPT
/* number of events for writer to give up the cpu */
static int resched_interval = 5000;
#endif

static struct completion read_start;

static struct task_struct *consumer;
static unsigned long iter_read;
static unsigned long long global_read;

static int *writer_finish;
static struct task_struct *producer;	/* Dispatch thread */
static struct task_struct **producers;
static struct completion *write_start;
static struct completion *write_done;
static atomic_long_t tot_hit;
static atomic_long_t tot_missed;

static int disable_reader;
module_param(disable_reader, uint, 0644);
MODULE_PARM_DESC(disable_reader, "only run producer");

static int file_reader;
module_param(file_reader, uint, 0644);
MODULE_PARM_DESC(file_reader, "open debugfs file for read()");

static unsigned int nr_producers = 1;
module_param(nr_producers, uint, 0644);
MODULE_PARM_DESC(nr_producers, "number of producer threads");

static int writer_delay;
module_param(writer_delay, uint, 0644);
MODULE_PARM_DESC(writer_delay, "delay between writes, in ms");

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

static char payload[] = "Ring buffer test data.\n";

static void wait_to_die(void)
{
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
}

#ifdef RING_BUFFER_PER_CPU_LOCAL_TMPL

static int read_event(struct ring_buffer *buf)
{
	ssize_t len;
	char *read_payload;

	len = ring_buffer_get_next_record(channel, buf);
	if (len < 0)
		return len;
	WARN_ON_ONCE(len != sizeof(payload) - 1);
	read_payload = kmalloc(len + 1, GFP_KERNEL);
	if (!read_payload)
		return -ENOMEM;

	read_current_record(buf, read_payload);
	read_payload[len] = '\0';

	WARN_ON_ONCE(strcmp(read_payload, payload));
	iter_read++;
	global_read++;
	kfree(read_payload);

	return 0;
}

static int read_channel_events(void)
{
	int cpu;
	int all_finalized = 1;
	int all_empty = 1;

	for_each_channel_cpu(cpu, channel) {
		struct ring_buffer *buf;
		int found = 1;

		buf = channel_get_ring_buffer(channel->backend.config, channel,
					      cpu);
		do {
			int ret;

			ret = read_event(buf);
			if (ret != -ENODATA)
				all_finalized = 0;
			if (ret == 0)
				all_empty = 0;
			else
				found = 0;
		} while (found && !kill_test);
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
		ret = read_channel_events();
		if (ret == -EAGAIN)
			wait_event_interruptible(channel->read_wait,
				(ret = read_channel_events(),
				 ret != -EAGAIN));
	} while (!kill_test && ret != -ENODATA);
}

#else /* !RING_BUFFER_PER_CPU_LOCAL_TMPL */

static int read_event(struct channel *chan)
{
	struct ring_buffer *buf;
	ssize_t len;
	char *read_payload;

	len = channel_get_next_record(chan, &buf);
	if (len < 0)
		return len;
	WARN_ON_ONCE(len != sizeof(payload) - 1);
	read_payload = kmalloc(len + 1, GFP_KERNEL);
	if (!read_payload)
		return -ENOMEM;

	read_current_record(buf, read_payload);
	read_payload[len] = '\0';

	WARN_ON_ONCE(strcmp(read_payload, payload));
	iter_read++;
	global_read++;
	kfree(read_payload);

	return 0;
}

static void ring_buffer_consumer(void)
{
	int ret;

	do {
		ret = read_event(channel);
		if (ret == -EAGAIN)
			wait_event_interruptible(channel->read_wait,
				(ret = read_event(channel),
				 ret != -EAGAIN));
	} while (!kill_test && ret != -ENODATA);
}

#endif /* !RING_BUFFER_PER_CPU_LOCAL_TMPL */

static int ring_buffer_consumer_thread(void *arg)
{
	WARN_ON_ONCE(channel_iterator_open(channel));

	wait_for_completion_interruptible(&read_start);

	ring_buffer_consumer();

	channel_iterator_release(channel);

	wait_to_die();

	return 0;
}

static void ring_buffer_producer(unsigned int writer_id)
{
	unsigned long long hit = 0;
	unsigned long long missed = 0;
	int cnt = 0;
	int ret;

	/*
	 * Hammer the buffer for 10 secs (this may make the system stall)
	 */
	while (!writer_finish[writer_id] && !kill_test) {
		ret = RING_BUFFER_TMPL(write)(channel, payload,
					      sizeof(payload) - 1);
		if (ret)
			missed++;
		else
			hit++;
		cnt++;

		if (writer_delay)
			msleep(writer_delay);

#ifndef CONFIG_PREEMPT
		/*
		 * If we are a non preempt kernel, the 10 second run will
		 * stop everything while it runs. Instead, we will call
		 * cond_resched and also add any time that was lost by a
		 * reschedule.
		 */
		if (!(cnt % resched_interval))
			cond_resched();
#endif

	}
	writer_finish[writer_id] = 0;
	atomic_long_add(hit, &tot_hit);
	atomic_long_add(missed, &tot_missed);
	complete(&write_done[writer_id]);
}

static void ring_buffer_report(unsigned long long time)
{
	unsigned long long hit, missed;
	unsigned long long written = 0;
	unsigned long long lost_full = 0, lost_wrap = 0, lost_big = 0;
	unsigned long long entries = 0;
	unsigned long long overruns = 0;
	unsigned long long read = 0;
	unsigned long avg;
	struct ring_buffer *buf;
	const struct ring_buffer_config *config;
#ifndef RING_BUFFER_GLOBAL_TMPL
	int cpu;
#endif

	config = channel->backend.config;
	buf = channel_get_ring_buffer(config, channel, 0);

	hit = atomic_long_read(&tot_hit);
	missed = atomic_long_read(&tot_missed);

	/*
	 * These values only take into account flushed subbuffers.
	 */
#ifdef RING_BUFFER_GLOBAL_TMPL
	written += ring_buffer_get_records_count(config, buf);
	lost_full += ring_buffer_get_records_lost_full(config, buf);
	lost_wrap += ring_buffer_get_records_lost_wrap(config, buf);
	lost_big += ring_buffer_get_records_lost_big(config, buf);
	overruns += ring_buffer_get_records_overrun(config, buf);
	entries += ring_buffer_get_records_unread(config, buf);
	read += ring_buffer_get_records_read(config, buf);
#else
	for_each_channel_cpu(cpu, channel) {
		struct ring_buffer *buf =
			channel_get_ring_buffer(config, channel, cpu);

		written += ring_buffer_get_records_count(config, buf);
		lost_full += ring_buffer_get_records_lost_full(config, buf);
		lost_wrap += ring_buffer_get_records_lost_wrap(config, buf);
		lost_big += ring_buffer_get_records_lost_big(config, buf);
		overruns += ring_buffer_get_records_overrun(config, buf);
		entries += ring_buffer_get_records_unread(config, buf);
		read += ring_buffer_get_records_read(config, buf);
	}
#endif

	trace_printk("Report for %s\n", RING_BUFFER_NAME_TMPL);

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
		trace_printk("WARNING!!! This test is running at lowest "
			     "priority.\n");

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

	/* Convert time from usecs to CPU time millisecs */
	time *= nr_producers;
	do_div(time, USEC_PER_MSEC);
	if (time)
		hit /= (long)time;
	else
		trace_printk("TIME IS ZERO??\n");

	trace_printk("Entries per millisec: %llu\n", hit);

	if (hit) {
		/* Calculate the average time in nanosecs */
		avg = NSEC_PER_MSEC / hit;
		trace_printk("%lu ns CPU time per entry written "
			     "(%u writer threads)\n",
			     avg, nr_producers);
	}

	if (missed) {
		if (time)
			missed /= (long)time;

		trace_printk("Total iterations per millisec: %llu\n",
			     hit + missed);

		/* it is possible that hit + missed will overflow and be zero */
		if (!(hit + missed)) {
			trace_printk("hit + missed overflowed and "
				     "totalled zero!\n");
			hit--; /* make it non zero */
		}

		/* Caculate the average time in nanosecs */
		avg = NSEC_PER_MSEC / (hit + missed);
		trace_printk("%lu ns CPU time per entry (written+lost) "
			     "(%u writer threads)\n",
			     avg, nr_producers);
	}
	iter_read = 0;
}

static int ring_buffer_producer_thread(void *arg)
{
	unsigned int writer_id = (unsigned long) arg;

	wait_for_completion_interruptible(&write_start[writer_id]);
	while (!kthread_should_stop() && !kill_test) {
		ring_buffer_producer(writer_id);
		wait_for_completion_interruptible(&write_start[writer_id]);
	}
	__set_current_state(TASK_RUNNING);

	if (kill_test)
		wait_to_die();

	return 0;
}

static int ring_buffer_main_producer_thread(void *arg)
{

	struct timeval start_tv;
	struct timeval end_tv;
	unsigned long long time;
	int i;

	if (consumer)
		complete(&read_start);

	while (!kthread_should_stop() && !kill_test) {
		trace_printk("Starting ring buffer hammer\n");
		do_gettimeofday(&start_tv);
		/* Wake up producers */
		for (i = 0; i < nr_producers; i++)
			complete(&write_start[i]);

		/* the completions must be visible before the finish var */
		smp_wmb();

		/* Wait for RUN_TIME  */
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ * RUN_TIME);
		__set_current_state(TASK_RUNNING);

		/* Stop producers */
		for (i = 0; i < nr_producers; i++)
			writer_finish[i] = 1;
		/* finish var visible before waking up */
		smp_wmb();

		/* Wait for producers to complete */
		for (i = 0; i < nr_producers; i++)
			wake_up_process(producers[i]);
		for (i = 0; i < nr_producers; i++)
			wait_for_completion_interruptible(&write_done[i]);
		do_gettimeofday(&end_tv);
		trace_printk("End ring buffer hammer\n");

		time = end_tv.tv_sec - start_tv.tv_sec;
		time *= USEC_PER_SEC;
		time += (long long)((long)end_tv.tv_usec
				    - (long)start_tv.tv_usec);

		if (kthread_should_stop() || kill_test)
			break;

		/* Print report */
		ring_buffer_report(time);
		atomic_long_set(&tot_hit, 0);
		atomic_long_set(&tot_missed, 0);

		trace_printk("Sleeping for 10 secs\n");
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ * SLEEP_TIME);
		__set_current_state(TASK_RUNNING);
	}

	if (kill_test)
		wait_to_die();

	return 0;
}

#ifdef RING_BUFFER_PER_CPU_LOCAL_TMPL

static int on_buffer_create(struct ring_buffer *buf, int cpu)
{
	int ret = 0;
	char *tmpname;

	if (!file_reader)
		return 0;

	tmpname = kzalloc(NAME_MAX + 1, GFP_KERNEL);
	if (!tmpname) {
		ret = -ENOMEM;
		goto end;
	}

	snprintf(tmpname, NAME_MAX, "%s_%d",
		 __stringify(RING_BUFFER_TMPL(benchmark)), cpu);
	dentry[cpu] = debugfs_create_file(tmpname, S_IRUSR,
					  NULL, buf,
					  &ring_buffer_payload_file_operations);
	if (!dentry[cpu])
		ret = -ENOMEM;
	kfree(tmpname);
end:
	return ret;
}

static void on_buffer_finalize(struct ring_buffer *buf, int cpu)
{
	if (file_reader)
		debugfs_remove(dentry[cpu]);
}

#endif /* RING_BUFFER_PER_CPU_LOCAL_TMPL */

static int __init ring_buffer_benchmark_init(void)
{
	int ret;
	unsigned int i;

#ifdef RING_BUFFER_PER_CPU_LOCAL_TMPL
	channel = RING_BUFFER_TMPL(create)(BUFFER_SIZE, on_buffer_create,
					   on_buffer_finalize);
#else
	channel = RING_BUFFER_TMPL(create)(BUFFER_SIZE);
#endif
	if (!channel)
		return -EINVAL;

	if (file_reader && !disable_reader) {
		printk(KERN_WARNING "Forcefully disabling reader; file "
				    "descriptor available for reading.\n");
		disable_reader = 1;
	}

	if (!disable_reader) {
		init_completion(&read_start);
		consumer = kthread_run(ring_buffer_consumer_thread,
				       NULL, "rb_consumer");
		ret = PTR_ERR(consumer);
		if (IS_ERR(consumer))
			goto out_fail;
	}

#ifndef RING_BUFFER_PER_CPU_LOCAL_TMPL
	if (file_reader) {
		dentry = debugfs_create_file(
				__stringify(RING_BUFFER_TMPL(benchmark)),
				S_IRUSR, NULL, channel,
				&channel_payload_file_operations);
		WARN_ON(!dentry);
	}
#endif

	producers = kzalloc(sizeof(struct task_struct *) * nr_producers,
			    GFP_KERNEL);
	writer_finish = kzalloc(sizeof(int) * nr_producers, GFP_KERNEL);
	write_start = kzalloc(sizeof(struct completion) * nr_producers,
			      GFP_KERNEL);
	write_done = kzalloc(sizeof(struct completion) * nr_producers,
			     GFP_KERNEL);
	if (!producers || !writer_finish || !write_start || !write_done) {
		ret = -ENOMEM;
		goto out_free_writer_structures;
	}

	for (i = 0; i < nr_producers; i++) {
		init_completion(&write_start[i]);
		init_completion(&write_done[i]);
		producers[i] = kthread_run(ring_buffer_producer_thread,
					   (void *)(unsigned long)i,
					   "rb_producer");
		ret = PTR_ERR(producers[i]);

		if (IS_ERR(producers[i]))
			goto out_kill_producers;
	}

	producer = kthread_run(ring_buffer_main_producer_thread,
			       NULL, "rb_main_producer");
	ret = PTR_ERR(producer);

	if (IS_ERR(producer))
		goto out_kill_producers;

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
		for (i = 0; i < nr_producers; i++)
			sched_setscheduler(producers[i], SCHED_FIFO, &param);
	} else {
		set_user_nice(producer, producer_nice);
		for (i = 0; i < nr_producers; i++)
			set_user_nice(producers[i], producer_nice);
	}

	return 0;

out_kill_producers:
	for (i = 0; i < nr_producers; i++)
		if (producers[i] && !IS_ERR(producers[i]))
			kthread_kill_stop(producers[i], SIGKILL);
out_free_writer_structures:
	kfree(write_done);
	kfree(write_start);
	kfree(writer_finish);
	kfree(producers);
#ifndef RING_BUFFER_PER_CPU_LOCAL_TMPL
	if (file_reader)
		debugfs_remove(dentry);
#endif
	if (consumer)
		kthread_kill_stop(consumer, SIGKILL);
out_fail:
	RING_BUFFER_TMPL(destroy)(channel);
	return ret;
}

static void __exit ring_buffer_benchmark_exit(void)
{
	unsigned int i;

	kthread_kill_stop(producer, SIGKILL);
	for (i = 0; i < nr_producers; i++)
		kthread_kill_stop(producers[i], SIGKILL);
#ifndef RING_BUFFER_PER_CPU_LOCAL_TMPL
	if (file_reader)
		debugfs_remove(dentry);
#endif
	RING_BUFFER_TMPL(destroy)(channel);
	if (consumer)
		kthread_kill_stop(consumer, SIGKILL);
	kfree(write_done);
	kfree(write_start);
	kfree(writer_finish);
	kfree(producers);
}

module_init(ring_buffer_benchmark_init);
module_exit(ring_buffer_benchmark_exit);
