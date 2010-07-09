/*
 * ring_buffer_percpu.c
 *
 * Copyright (C) 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Per-CPU ring buffer library implementation.
 * Creates instances of both overwrite and discard modes.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/ringbuffer/config.h>
#include <linux/ringbuffer/percpu_overwrite.h>
#include <linux/ringbuffer/percpu_discard.h>
#include <linux/ringbuffer/vfs.h>
#include <linux/prio_heap.h>
#include <linux/trace_clock.h>

struct subbuffer_header {
	uint8_t header_end[0];		/* End of header */
};

struct record_header {
	uint64_t timestamp;		/* Record timestamp */
	uint32_t len;			/* Size of record payload */
	uint8_t header_end[0];		/* End of header */
};

static inline
u64 ring_buffer_clock_read(struct channel *chan)
{
	return trace_clock();
}

static inline
unsigned char record_header_size(const struct ring_buffer_config *config,
				 struct channel *chan, size_t offset,
				 size_t data_size, size_t *pre_header_padding,
				 unsigned int rflags,
				 struct ring_buffer_ctx *ctx)
{
	return offsetof(struct record_header, header_end);
}

#include <linux/ringbuffer/api.h>

static
u64 client_ring_buffer_clock_read(struct channel *chan)
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

static
size_t client_subbuffer_header_size(void)
{
	return offsetof(struct subbuffer_header, header_end);
}

static
void client_buffer_begin(struct ring_buffer *buf, u64 tsc,
			 unsigned int subbuf_idx)
{
}

static
void client_buffer_end(struct ring_buffer *buf, u64 tsc,
		       unsigned int subbuf_idx, unsigned long data_size)
{
}

static
int client_buffer_create(struct ring_buffer *buf, void *priv,
			 int cpu, const char *name)
{
	return 0;
}

static
void client_buffer_finalize(struct ring_buffer *buf, void *priv, int cpu)
{
}

static
void client_record_get(const struct ring_buffer_config *config,
		       struct channel *chan, struct ring_buffer *buf,
		       size_t offset, size_t *header_len,
		       size_t *payload_len, u64 *timestamp)
{
	int ret;
	struct record_header header;

	ret = ring_buffer_read(&buf->backend, offset, &header,
			       offsetof(struct record_header, header_end));
	CHAN_WARN_ON(chan, ret != offsetof(struct record_header, header_end));
	*header_len = offsetof(struct record_header, header_end);
	*payload_len = header.len;
	*timestamp = header.timestamp;
}

/*
 * Typically 8 subbuffers of variable size per CPU. We allocate more than 2
 * subbuffers per cpu to provide room for the merge-sort.
 * Maximum subbuffer size is 4GB. Allocate more subbuffers if more space is
 * requested.
 * Periodical buffer switch deferrable timer is set to 100ms. This will wake up
 * blocking reads when partially filled subbuffers are ready for reading.
 * Periodical reader wakeup delivery timer is disabled. It is useless because
 * RING_BUFFER_WAKEUP_BY_WRITER is set.
 */
#define SP_SUBBUF_NUM_ORDER	3
#define SP_SUBBUF_NUM		(1 << SP_SUBBUF_NUM_ORDER)
#define SP_SWITCH_INTERVAL_MS	100U
#define SP_SWITCH_INTERVAL_US	(SP_SWITCH_INTERVAL_MS * 1000)
#define SP_READ_INTERVAL_US	0
#define SP_U32_MAX		4294967295U	/* 2^32 - 1 */

static const struct ring_buffer_config percpu_overwrite_config = {
	.cb.ring_buffer_clock_read = client_ring_buffer_clock_read,
	.cb.record_header_size = client_record_header_size,
	.cb.subbuffer_header_size = client_subbuffer_header_size,
	.cb.buffer_begin = client_buffer_begin,
	.cb.buffer_end = client_buffer_end,
	.cb.buffer_create = client_buffer_create,
	.cb.buffer_finalize = client_buffer_finalize,
	.cb.record_get = client_record_get,

	.tsc_bits = 64,
	.alloc = RING_BUFFER_ALLOC_PER_CPU,
	.sync = RING_BUFFER_SYNC_PER_CPU,
	.mode = RING_BUFFER_OVERWRITE,
	.align = RING_BUFFER_PACKED,
	.backend = RING_BUFFER_PAGE,
	.output = RING_BUFFER_ITERATOR,
	.oops = RING_BUFFER_NO_OOPS_CONSISTENCY,
	.ipi = RING_BUFFER_IPI_BARRIER,
	.wakeup = RING_BUFFER_WAKEUP_BY_WRITER,
};

static const struct ring_buffer_config percpu_discard_config = {
	.cb.ring_buffer_clock_read = client_ring_buffer_clock_read,
	.cb.record_header_size = client_record_header_size,
	.cb.subbuffer_header_size = client_subbuffer_header_size,
	.cb.buffer_begin = client_buffer_begin,
	.cb.buffer_end = client_buffer_end,
	.cb.buffer_create = client_buffer_create,
	.cb.buffer_finalize = client_buffer_finalize,
	.cb.record_get = client_record_get,

	.tsc_bits = 64,
	.alloc = RING_BUFFER_ALLOC_PER_CPU,
	.sync = RING_BUFFER_SYNC_PER_CPU,
	.mode = RING_BUFFER_DISCARD,
	.align = RING_BUFFER_PACKED,
	.backend = RING_BUFFER_PAGE,
	.output = RING_BUFFER_ITERATOR,
	.oops = RING_BUFFER_NO_OOPS_CONSISTENCY,
	.ipi = RING_BUFFER_IPI_BARRIER,
	.wakeup = RING_BUFFER_WAKEUP_BY_WRITER,
};

/* Wrapper library API */

static
struct channel *ring_buffer_percpu_create(
				const struct ring_buffer_config *config,
				size_t buf_size)
{
	size_t subbuf_size, subbuf_size_order;
	unsigned int subbuf_num = SP_SUBBUF_NUM;

	/* Typically use 8 subbuffers, minimum of PAGE_SIZE size each */
	buf_size = max_t(size_t, buf_size, PAGE_SIZE << SP_SUBBUF_NUM_ORDER);
	subbuf_size = buf_size >> SP_SUBBUF_NUM_ORDER;
	/*
	 * Ensure the event payload size fits on u32 event header.
	 * Maximum subbuffer size is therefore 4GB.
	 */
	subbuf_size = min_t(size_t, SP_U32_MAX, subbuf_size);

	/* Allocate more than 8 subbuffers if necessary. */
	if (subbuf_size < (buf_size >> SP_SUBBUF_NUM_ORDER)) {
		subbuf_size_order = get_count_order(subbuf_size);
		subbuf_num = buf_size >> subbuf_size_order;
	}

	return channel_create(config, "sp", NULL, NULL,
			      subbuf_size, subbuf_num,
			      SP_SWITCH_INTERVAL_US, SP_READ_INTERVAL_US);
}

/**
 * ring_buffer_percpu_overwrite_create - creates a per-cpu overwrite ring
 *                                       buffer.
 * @buf_size: the buffer size
 *
 * Returns the ring buffer channel address on success, NULL on error.
 */
struct channel *ring_buffer_percpu_overwrite_create(size_t buf_size)
{
	return ring_buffer_percpu_create(&percpu_overwrite_config, buf_size);
}
EXPORT_SYMBOL_GPL(ring_buffer_percpu_overwrite_create);

/**
 * ring_buffer_percpu_discard_create - creates a per-cpu discard ring buffer.
 * @buf_size: the buffer size
 *
 * Returns the ring buffer channel address on success, NULL on error.
 */

struct channel *ring_buffer_percpu_discard_create(size_t buf_size)
{
	return ring_buffer_percpu_create(&percpu_discard_config, buf_size);
}
EXPORT_SYMBOL_GPL(ring_buffer_percpu_discard_create);

static
void ring_buffer_percpu_destroy(struct channel *chan)
{
	channel_destroy(chan);
}

/**
 * ring_buffer_percpu_overwrite_destroy - deletes a per-cpu
 *                                        overwrite ring buffer.
 * @chan: ring buffer channel
 */
void ring_buffer_percpu_overwrite_destroy(struct channel *chan)
{
	ring_buffer_percpu_destroy(chan);
}
EXPORT_SYMBOL_GPL(ring_buffer_percpu_overwrite_destroy);

/**
 * ring_buffer_percpu_discard_destroy - deletes a per-cpu
 *                                      discard ring buffer.
 * @chan: ring buffer channel
 */
void ring_buffer_percpu_discard_destroy(struct channel *chan)
{
	ring_buffer_percpu_destroy(chan);
}
EXPORT_SYMBOL_GPL(ring_buffer_percpu_discard_destroy);

/**
 * ring_buffer_percpu_write - writes a record into the ring buffer.
 * @chan: ring buffer channel
 * @src: start of input to copy from
 * @len: length of record
 *
 * The record starts at the "src" address and is "len" bytes long. Returns 0 on
 * success, else it returns a negative error value.
 */
static
int ring_buffer_percpu_write(const struct ring_buffer_config *config,
			     struct channel *chan, const void *src, size_t len)
{
	struct percpu_private *priv = channel_get_private(chan);
	struct record_header header;
	struct ring_buffer_ctx ctx;
	int ret, cpu;

	cpu = ring_buffer_get_cpu(config);
	if (cpu < 0) {
		ret = cpu;
		goto end;
	}
	ring_buffer_ctx_init(&ctx, chan, priv, len, 0, cpu);
	ret = ring_buffer_reserve(config, &ctx);
	if (ret)
		goto put;
	header.timestamp = ctx.tsc;
	header.len = len;
	ring_buffer_write(config, &ctx, &header,
			  offsetof(struct record_header, header_end));
	ring_buffer_write(config, &ctx, src, len);
	ring_buffer_commit(config, &ctx);
put:
	ring_buffer_put_cpu(config);
end:
	return ret;
}

/**
 * ring_buffer_percpu_overwrite_write - writes a record into the ring buffer.
 * @chan: ring buffer channel
 * @src: start of input to copy from
 * @len: length of record
 *
 * The record starts at the "src" address and is "len" bytes long. Returns 0 on
 * success, else it returns a negative error value.
 */
int ring_buffer_percpu_overwrite_write(struct channel *chan, const void *src,
				       size_t len)
{
	return ring_buffer_percpu_write(&percpu_overwrite_config, chan, src,
					len);
}
EXPORT_SYMBOL_GPL(ring_buffer_percpu_overwrite_write);

/**
 * ring_buffer_percpu_discard_write - writes a record into the ring buffer.
 * @chan: ring buffer channel
 * @src: start of input to copy from
 * @len: length of record
 *
 * The record starts at the "src" address and is "len" bytes long. Returns 0 on
 * success, else it returns a negative error value.
 */
int ring_buffer_percpu_discard_write(struct channel *chan, const void *src,
				     size_t len)
{
	return ring_buffer_percpu_write(&percpu_discard_config, chan, src,
					len);
}
EXPORT_SYMBOL_GPL(ring_buffer_percpu_discard_write);

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Mathieu Desnoyers");
MODULE_DESCRIPTION("Ring Buffer Library Per-CPU Client");
