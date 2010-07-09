/*
 * ring_buffer_global.c
 *
 * Copyright (C) 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer global library implementation.
 * Creates instances of both overwrite and discard modes.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/ringbuffer/config.h>
#include <linux/ringbuffer/global_overwrite.h>
#include <linux/ringbuffer/global_discard.h>
#include <linux/ringbuffer/vfs.h>

struct subbuffer_header {
	uint8_t header_end[0];		/* End of header */
};

struct record_header {
	uint32_t len;			/* Size of record payload */
	uint8_t header_end[0];		/* End of header */
};

static inline
u64 ring_buffer_clock_read(struct channel *chan)
{
	return 0;
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
	return 0;
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
	struct record_header header;
	int ret;

	ret = ring_buffer_read(&buf->backend, offset, &header,
			       offsetof(struct record_header, header_end));
	CHAN_WARN_ON(chan, ret != offsetof(struct record_header, header_end));
	*header_len = offsetof(struct record_header, header_end);
	*payload_len = header.len;
	*timestamp = 0;
}

/*
 * Typically 8 subbuffers of variable size.
 * Maximum subbuffer size is 4GB. Allocate more subbuffers if more space is
 * requested.
 * Periodical buffer switch deferrable timer set to 100ms.
 * Periodical reader wakeup delivery timer is disabled. It is useless because
 * RING_BUFFER_WAKEUP_BY_WRITER is set.
 */
#define SG_SUBBUF_NUM_ORDER	3
#define SG_SUBBUF_NUM		(1 << SG_SUBBUF_NUM_ORDER)
#define SG_SWITCH_INTERVAL_MS	100U
#define SG_SWITCH_INTERVAL_US	(SG_SWITCH_INTERVAL_MS * 1000)
#define SG_READ_INTERVAL_US	0
#define SG_U32_MAX		4294967295U	/* 2^32 - 1 */

static const struct ring_buffer_config global_overwrite_config = {
	.cb.ring_buffer_clock_read = client_ring_buffer_clock_read,
	.cb.record_header_size = client_record_header_size,
	.cb.subbuffer_header_size = client_subbuffer_header_size,
	.cb.buffer_begin = client_buffer_begin,
	.cb.buffer_end = client_buffer_end,
	.cb.buffer_create = client_buffer_create,
	.cb.buffer_finalize = client_buffer_finalize,
	.cb.record_get = client_record_get,

	.tsc_bits = 0,
	.alloc = RING_BUFFER_ALLOC_GLOBAL,
	.sync = RING_BUFFER_SYNC_GLOBAL,
	.mode = RING_BUFFER_OVERWRITE,
	.align = RING_BUFFER_PACKED,
	.backend = RING_BUFFER_PAGE,
	.output = RING_BUFFER_ITERATOR,
	.oops = RING_BUFFER_NO_OOPS_CONSISTENCY,
	.ipi = RING_BUFFER_NO_IPI_BARRIER,
	.wakeup = RING_BUFFER_WAKEUP_BY_WRITER,
};

static const struct ring_buffer_config global_discard_config = {
	.cb.ring_buffer_clock_read = client_ring_buffer_clock_read,
	.cb.record_header_size = client_record_header_size,
	.cb.subbuffer_header_size = client_subbuffer_header_size,
	.cb.buffer_begin = client_buffer_begin,
	.cb.buffer_end = client_buffer_end,
	.cb.buffer_create = client_buffer_create,
	.cb.buffer_finalize = client_buffer_finalize,
	.cb.record_get = client_record_get,

	.tsc_bits = 0,
	.alloc = RING_BUFFER_ALLOC_GLOBAL,
	.sync = RING_BUFFER_SYNC_GLOBAL,
	.mode = RING_BUFFER_DISCARD,
	.align = RING_BUFFER_PACKED,
	.backend = RING_BUFFER_PAGE,
	.output = RING_BUFFER_ITERATOR,
	.oops = RING_BUFFER_NO_OOPS_CONSISTENCY,
	.ipi = RING_BUFFER_NO_IPI_BARRIER,
	.wakeup = RING_BUFFER_WAKEUP_BY_WRITER,
};

/* Wrapper library API */

static
struct channel *ring_buffer_global_create(
				const struct ring_buffer_config *config,
				size_t buf_size)
{
	size_t subbuf_size, subbuf_size_order;
	unsigned int subbuf_num = SG_SUBBUF_NUM;

	/* Typically use 8 subbuffers, minimum of PAGE_SIZE size each */
	buf_size = max_t(size_t, buf_size, PAGE_SIZE << SG_SUBBUF_NUM_ORDER);
	subbuf_size = buf_size >> SG_SUBBUF_NUM_ORDER;
	/*
	 * Ensure the event payload size fits on u32 event header.
	 * Maximum subbuffer size is therefore 4GB.
	 */
	subbuf_size = min_t(size_t, SG_U32_MAX, subbuf_size);

	/* Allocate more than 8 subbuffers if necessary. */
	if (subbuf_size < (buf_size >> SG_SUBBUF_NUM_ORDER)) {
		subbuf_size_order = get_count_order(subbuf_size);
		subbuf_num = buf_size >> subbuf_size_order;
	}

	return channel_create(config, "sg", NULL, NULL,
			      subbuf_size, subbuf_num,
			      SG_SWITCH_INTERVAL_US, SG_READ_INTERVAL_US);
}

/**
 * ring_buffer_global_overwrite_create - creates a global overwrite ring buffer.
 * @buf_size: the buffer size
 *
 * Returns the ring buffer channel address on success, NULL on error.
 */
struct channel *ring_buffer_global_overwrite_create(size_t buf_size)
{
	return ring_buffer_global_create(&global_overwrite_config, buf_size);
}
EXPORT_SYMBOL_GPL(ring_buffer_global_overwrite_create);

/**
 * ring_buffer_global_discard_create - creates a global discard ring buffer.
 * @buf_size: the buffer size
 *
 * Returns the ring buffer channel address on success, NULL on error.
 */

struct channel *ring_buffer_global_discard_create(size_t buf_size)
{
	return ring_buffer_global_create(&global_discard_config, buf_size);
}
EXPORT_SYMBOL_GPL(ring_buffer_global_discard_create);

static
void ring_buffer_global_destroy(struct channel *chan)
{
	channel_destroy(chan);
}

/**
 * ring_buffer_global_overwrite_destroy - teardown global overwrite ring buffer.
 * @chan: ring buffer channel
 */
void ring_buffer_global_overwrite_destroy(struct channel *chan)
{
	ring_buffer_global_destroy(chan);
}
EXPORT_SYMBOL_GPL(ring_buffer_global_overwrite_destroy);

/**
 * ring_buffer_global_overwrite_destroy - teardown global discard ring buffer.
 * @chan: ring buffer channel
 */
void ring_buffer_global_discard_destroy(struct channel *chan)
{
	ring_buffer_global_destroy(chan);
}
EXPORT_SYMBOL_GPL(ring_buffer_global_discard_destroy);

/**
 * ring_buffer_global_write - writes a record into the ring buffer.
 * @chan: ring buffer channel
 * @src: start of input to copy from
 * @len: length of record
 *
 * The record starts at the "src" address and is "len" bytes long. Returns 0 on
 * success, else it returns a negative error value.
 */
static
int ring_buffer_global_write(const struct ring_buffer_config *config,
			     struct channel *chan, const void *src, size_t len)
{
	struct record_header header;
	struct ring_buffer_ctx ctx;
	int ret;

	ring_buffer_ctx_init(&ctx, chan, NULL, len, 0, 0);
	ret = ring_buffer_reserve(config, &ctx);
	if (ret)
		goto end;
	header.len = len;
	ring_buffer_write(config, &ctx, &header,
			  offsetof(struct record_header, header_end));
	ring_buffer_write(config, &ctx, src, len);
	ring_buffer_commit(config, &ctx);
end:
	return ret;

}

/**
 * ring_buffer_global_overwrite_write - writes a record into the ring buffer.
 * @chan: ring buffer channel
 * @src: start of input to copy from
 * @len: length of record
 *
 * The record starts at the "src" address and is "len" bytes long. Returns 0 on
 * success, else it returns a negative error value.
 */
int ring_buffer_global_overwrite_write(struct channel *chan, const void *src,
				       size_t len)
{
	return ring_buffer_global_write(&global_overwrite_config, chan, src,
					len);
}
EXPORT_SYMBOL_GPL(ring_buffer_global_overwrite_write);

/**
 * ring_buffer_global_discard_write - writes a record into the ring buffer.
 * @chan: ring buffer channel
 * @src: start of input to copy from
 * @len: length of record
 *
 * The record starts at the "src" address and is "len" bytes long. Returns 0 on
 * success, else it returns a negative error value.
 */
int ring_buffer_global_discard_write(struct channel *chan, const void *src,
				     size_t len)
{
	return ring_buffer_global_write(&global_discard_config, chan, src, len);
}
EXPORT_SYMBOL_GPL(ring_buffer_global_discard_write);

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Mathieu Desnoyers");
MODULE_DESCRIPTION("Ring Buffer Library Global Client");
