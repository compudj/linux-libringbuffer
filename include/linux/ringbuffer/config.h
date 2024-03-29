#ifndef _LINUX_RING_BUFFER_CONFIG_H
#define _LINUX_RING_BUFFER_CONFIG_H

/*
 * linux/ringbuffer/config.h
 *
 * Copyright (C) 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer configuration header. Note: after declaring the standard inline
 * functions, clients should also include linux/ringbuffer/api.h.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/types.h>
#include <linux/percpu.h>

struct ring_buffer;
struct channel;
struct ring_buffer_config;
struct ring_buffer_ctx;

/*
 * Ring buffer client callbacks. Only used by slow path, never on fast path.
 * For the fast path, record_header_size(), ring_buffer_clock_read() should be
 * provided as inline functions too.  These may simply return 0 if not used by
 * the client.
 */
struct ring_buffer_client_cb {
	/* Mandatory callbacks */

	/* A static inline version is also required for fast path */
	u64 (*ring_buffer_clock_read) (struct channel *chan);
	size_t (*record_header_size) (const struct ring_buffer_config *config,
				      struct channel *chan, size_t offset,
				      size_t data_size,
				      size_t *pre_header_padding,
				      unsigned int rflags,
				      struct ring_buffer_ctx *ctx);

	/* Slow path only, at subbuffer switch */
	size_t (*subbuffer_header_size) (void);
	void (*buffer_begin) (struct ring_buffer *buf, u64 tsc,
			      unsigned int subbuf_idx);
	void (*buffer_end) (struct ring_buffer *buf, u64 tsc,
			    unsigned int subbuf_idx, unsigned long data_size);

	/* Optional callbacks (can be set to NULL) */

	/* Called at buffer creation/finalize */
	int (*buffer_create) (struct ring_buffer *buf, void *priv,
			      int cpu, const char *name);
	/*
	 * Clients should guarantee that no new reader handle can be opened
	 * after finalize.
	 */
	void (*buffer_finalize) (struct ring_buffer *buf, void *priv, int cpu);

	/*
	 * Extract header length, payload length and timestamp from event
	 * record. Used by buffer iterators. Timestamp is only used by channel
	 * iterator.
	 */
	void (*record_get) (const struct ring_buffer_config *config,
			    struct channel *chan, struct ring_buffer *buf,
			    size_t offset, size_t *header_len,
			    size_t *payload_len, u64 *timestamp);
};

/*
 * Ring buffer instance configuration.
 *
 * Declare as "static const" within the client object to ensure the inline fast
 * paths can be optimized.
 *
 * alloc/sync pairs:
 *
 * RING_BUFFER_ALLOC_PER_CPU and RING_BUFFER_SYNC_PER_CPU :
 *   Per-cpu buffers with per-cpu synchronization. Tracing must be performed
 *   with preemption disabled (ring_buffer_get_cpu() and ring_buffer_put_cpu()).
 *
 * RING_BUFFER_ALLOC_PER_CPU and RING_BUFFER_SYNC_GLOBAL :
 *   Per-cpu buffer with global synchronization. Tracing can be performed with
 *   preemption enabled, statistically stays on the local buffers.
 *
 * RING_BUFFER_ALLOC_GLOBAL and RING_BUFFER_SYNC_PER_CPU :
 *   Should only be used for buffers belonging to a single thread or protected
 *   by mutual exclusion by the client. Note that periodical sub-buffer switch
 *   should be disabled in this kind of configuration.
 *
 * RING_BUFFER_ALLOC_GLOBAL and RING_BUFFER_SYNC_GLOBAL :
 *   Global shared buffer with global synchronization.
 *
 * wakeup:
 *
 * RING_BUFFER_WAKEUP_BY_TIMER uses per-cpu deferrable timers to poll the
 * buffers and wake up readers if data is ready. Mainly useful for tracers which
 * don't want to call into the wakeup code on the tracing path. Use in
 * combination with "read_timer_interval" channel_create() argument.
 *
 * RING_BUFFER_WAKEUP_BY_WRITER directly wakes up readers when a subbuffer is
 * ready to read. Lower latencies before the reader is woken up. Mainly suitable
 * for drivers.
 *
 * RING_BUFFER_WAKEUP_NONE does not perform any wakeup whatsoever. The client
 * has the responsibility to perform wakeups.
 */
struct ring_buffer_config {
	enum {
		RING_BUFFER_ALLOC_PER_CPU,
		RING_BUFFER_ALLOC_GLOBAL,
	} alloc;
	enum {
		RING_BUFFER_SYNC_PER_CPU,	/* Wait-free */
		RING_BUFFER_SYNC_GLOBAL,	/* Lock-free */
	} sync;
	enum {
		RING_BUFFER_OVERWRITE,		/* Overwrite when buffer full */
		RING_BUFFER_DISCARD,		/* Discard when buffer full */
	} mode;
	enum {
		RING_BUFFER_NATURAL,
		RING_BUFFER_PACKED,
	} align;
	enum {
		RING_BUFFER_SPLICE,
		RING_BUFFER_MMAP,
		RING_BUFFER_READ,		/* TODO */
		RING_BUFFER_ITERATOR,
		RING_BUFFER_NONE,
	} output;
	enum {
		RING_BUFFER_PAGE,
		RING_BUFFER_VMAP,		/* TODO */
		RING_BUFFER_STATIC,		/* TODO */
	} backend;
	enum {
		RING_BUFFER_NO_OOPS_CONSISTENCY,
		RING_BUFFER_OOPS_CONSISTENCY,
	} oops;
	enum {
		RING_BUFFER_IPI_BARRIER,
		RING_BUFFER_NO_IPI_BARRIER,
	} ipi;
	enum {
		RING_BUFFER_WAKEUP_BY_TIMER,	/* wake up performed by timer */
		RING_BUFFER_WAKEUP_BY_WRITER,	/*
						 * writer wakes up reader,
						 * not lock-free
						 * (takes spinlock).
						 */
	} wakeup;
	/*
	 * tsc_bits: timestamp bits saved at each record.
	 *   0 and 64 disable the timestamp compression scheme.
	 */
	unsigned int tsc_bits;
	struct ring_buffer_client_cb cb;
};

/*
 * ring buffer context
 *
 * Context passed to ring_buffer_reserve(), ring_buffer_commit(),
 * ring_buffer_try_discard_reserve(), ring_buffer_align_ctx() and
 * ring_buffer_write().
 */
struct ring_buffer_ctx {
	/* input received by ring_buffer_reserve(), saved here. */
	struct channel *chan;		/* channel */
	void *priv;			/* client private data */
	size_t data_size;		/* size of payload */
	int largest_align;		/*
					 * alignment of the largest element
					 * in the payload
					 */
	int cpu;			/* processor id */

	/* output from ring_buffer_reserve() */
	struct ring_buffer *buf;	/*
					 * buffer corresponding to processor id
					 * for this channel
					 */
	size_t slot_size;		/* size of the reserved slot */
	unsigned long buf_offset;	/* offset following the record header */
	unsigned long pre_offset;	/*
					 * Initial offset position _before_
					 * the record is written. Positioned
					 * prior to record header alignment
					 * padding.
					 */
	u64 tsc;			/* time-stamp counter value */
	unsigned int rflags;		/* reservation flags */
};

/**
 * ring_buffer_ctx_init - initialize ring buffer context
 * @ctx: ring buffer context to initialize
 * @chan: channel
 * @priv: client private data
 * @data_size: size of record data payload
 * @largest_align: largest alignment within data payload types
 * @cpu: processor id
 */
static inline
void ring_buffer_ctx_init(struct ring_buffer_ctx *ctx,
			  struct channel *chan, void *priv,
			  size_t data_size, int largest_align,
			  int cpu)
{
	ctx->chan = chan;
	ctx->priv = priv;
	ctx->data_size = data_size;
	ctx->largest_align = largest_align;
	ctx->cpu = cpu;
}

/*
 * Reservation flags.
 *
 * RING_BUFFER_RFLAG_FULL_TSC
 *
 * This flag is passed to record_header_size() and to the primitive used to
 * write the record header. It indicates that the full 64-bit time value is
 * needed in the record header. If this flag is not set, the record header needs
 * only to contain "tsc_bits" bit of time value.
 *
 * Reservation flags can be added by the client, starting from
 * "(RING_BUFFER_FLAGS_END << 0)". It can be used to pass information from
 * record_header_size() to ring_buffer_write_record_header().
 */
#define	RING_BUFFER_RFLAG_FULL_TSC		(1U << 0)
#define RING_BUFFER_RFLAG_END			(1U << 1)

/*
 * We need to define RING_BUFFER_ALIGN_ATTR so it is known early at
 * compile-time. We have to duplicate the "config->align" information and the
 * definition here because config->align is used both in the slow and fast
 * paths, but RING_BUFFER_ALIGN_ATTR is only available for the client code.
 */
#ifdef RING_BUFFER_ALIGN
# define RING_BUFFER_ALIGN_ATTR		/* Default arch alignment */
#else
# define RING_BUFFER_ALIGN_ATTR __attribute__((packed))
#endif

/*
 * Calculate the offset needed to align the type.
 * size_of_type must be non-zero.
 */
static inline
unsigned int ring_buffer_align(const struct ring_buffer_config *config,
			       size_t align_drift, size_t size_of_type)
{
	switch (config->align) {
	case RING_BUFFER_NATURAL:
		return offset_align(align_drift, min(sizeof(void *),
						     size_of_type));
	case RING_BUFFER_PACKED:
	default:
		return 0;
	}
}

static inline
int ring_buffer_get_alignment(const struct ring_buffer_config *config)
{
	switch (config->align) {
	case RING_BUFFER_NATURAL:
		return sizeof(void *);
	case RING_BUFFER_PACKED:
	default:
		return 0;
	}
}

/**
 * ring_buffer_align_ctx - Align context offset on "alignment"
 * @config: ring buffer instance configuration.
 * @ctx: ring buffer context.
 */
static inline
void ring_buffer_align_ctx(const struct ring_buffer_config *config,
			   struct ring_buffer_ctx *ctx,
			   size_t alignment)
{
	ctx->buf_offset += ring_buffer_align(config, ctx->buf_offset,
					     alignment);
}

/*
 * ring_buffer_check_config() returns 0 on success.
 * Used internally to check for valid configurations at channel creation.
 */
static inline
int ring_buffer_check_config(const struct ring_buffer_config *config,
			     unsigned int switch_timer_interval,
			     unsigned int read_timer_interval)
{
	if (config->alloc == RING_BUFFER_ALLOC_GLOBAL
	    && config->sync == RING_BUFFER_SYNC_PER_CPU
	    && switch_timer_interval)
		return -EINVAL;
	return 0;
}

#include <linux/ringbuffer/vatomic.h>

#endif /* _LINUX_RING_BUFFER_CONFIG_H */
