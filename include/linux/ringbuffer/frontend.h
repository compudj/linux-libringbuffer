#ifndef _LINUX_RING_BUFFER_FRONTEND_H
#define _LINUX_RING_BUFFER_FRONTEND_H

/*
 * linux/ringbuffer/frontend.h
 *
 * (C) Copyright 2005-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring Buffer Library Synchronization Header (API).
 *
 * Author:
 *	Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * See ring_buffer_frontend.c for more information on wait-free algorithms.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/pipe_fs_i.h>
#include <linux/rcupdate.h>
#include <linux/smp_lock.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/splice.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/cache.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/stat.h>
#include <linux/cpu.h>
#include <linux/fs.h>

#include <asm/atomic.h>
#include <asm/local.h>

/* Internal helpers */
#include <linux/ringbuffer/frontend_internal.h>

/* Buffer creation/removal and setup operations */

/*
 * switch_timer_interval is the time interval (in us) to fill sub-buffers with
 * padding to let readers get those sub-buffers.  Used for live streaming.
 *
 * read_timer_interval is the time interval (in us) to wake up pending readers.
 *
 * buf_addr is a pointer the the beginning of the preallocated buffer contiguous
 * address mapping. It is used only by RING_BUFFER_STATIC configuration. It can
 * be set to NULL for other backends.
 */

extern
struct channel *channel_create(const struct ring_buffer_config *config,
			       const char *name, void *priv,
			       void *buf_addr,
			       size_t subbuf_size, size_t num_subbuf,
			       unsigned int switch_timer_interval,
			       unsigned int read_timer_interval);

/*
 * channel_destroy returns the private data pointer. It finalizes all channel's
 * buffers, waits for readers to release all references, and destroys the
 * channel.
 */
extern
void *channel_destroy(struct channel *chan);


/* Buffer read operations */

/*
 * Iteration on channel cpumask needs to issue a read barrier to match the write
 * barrier in cpu hotplug. It orders the cpumask read before read of per-cpu
 * buffer data. The per-cpu buffer is never removed by cpu hotplug; teardown is
 * only performed at channel destruction.
 */
#define for_each_channel_cpu(cpu, chan)					\
	for ((cpu) = -1;						\
		({ (cpu) = cpumask_next((cpu), (chan)->backend.cpumask);\
		   smp_read_barrier_depends(); (cpu) < nr_cpu_ids; });)

extern struct ring_buffer *channel_get_ring_buffer(
					const struct ring_buffer_config *config,
					struct channel *chan, int cpu);
extern int ring_buffer_open_read(struct ring_buffer *buf);
extern void ring_buffer_release_read(struct ring_buffer *buf);
extern int ring_buffer_get_subbuf(struct ring_buffer *buf,
				  unsigned long *consumed);
extern void ring_buffer_put_subbuf(struct ring_buffer *buf,
				   unsigned long consumed);

extern void channel_reset(struct channel *chan);
extern void ring_buffer_reset(struct ring_buffer *buf);

static inline
unsigned long ring_buffer_get_offset(const struct ring_buffer_config *config,
				     struct ring_buffer *buf)
{
	return v_read(config, &buf->offset);
}

static inline
unsigned long ring_buffer_get_consumed(const struct ring_buffer_config *config,
				       struct ring_buffer *buf)
{
	return atomic_long_read(&buf->consumed);
}

/*
 * Must call ring_buffer_is_finalized before reading counters (memory ordering
 * enforced with respect to trace teardown).
 */
static inline
int ring_buffer_is_finalized(const struct ring_buffer_config *config,
			     struct ring_buffer *buf)
{
	int finalized = ACCESS_ONCE(buf->finalized);
	/*
	 * Read finalized before counters.
	 */
	smp_rmb();
	return finalized;
}

static inline
unsigned long ring_buffer_get_read_data_size(
					const struct ring_buffer_config *config,
					struct ring_buffer *buf)
{
	return subbuffer_get_read_data_size(config, &buf->backend);
}

static inline
unsigned long ring_buffer_get_records_count(
				const struct ring_buffer_config *config,
				struct ring_buffer *buf)
{
	return v_read(config, &buf->records_count);
}

static inline
unsigned long ring_buffer_get_records_overrun(
				const struct ring_buffer_config *config,
				struct ring_buffer *buf)
{
	return v_read(config, &buf->records_overrun);
}

static inline
unsigned long ring_buffer_get_records_lost_full(
				const struct ring_buffer_config *config,
				struct ring_buffer *buf)
{
	return v_read(config, &buf->records_lost_full);
}

static inline
unsigned long ring_buffer_get_records_lost_wrap(
				const struct ring_buffer_config *config,
				struct ring_buffer *buf)
{
	return v_read(config, &buf->records_lost_wrap);
}

static inline
unsigned long ring_buffer_get_records_lost_big(
				const struct ring_buffer_config *config,
				struct ring_buffer *buf)
{
	return v_read(config, &buf->records_lost_big);
}

static inline
unsigned long ring_buffer_get_records_read(
				const struct ring_buffer_config *config,
				struct ring_buffer *buf)
{
	return v_read(config, &buf->backend.records_read);
}

static inline
void *channel_get_private(struct channel *chan)
{
	return chan->backend.priv;
}

#endif /* _LINUX_RING_BUFFER_FRONTEND_H */
