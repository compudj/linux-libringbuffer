/*
 * ring_buffer_template_user.c
 *
 * Copyright (C) 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer template instance code example.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/module.h>
#include <linux/debugfs.h>

#include "ring_buffer_template_user.h"

struct ring_buffer_priv {
	struct dentry *dentry;
};

struct channel_priv {
	struct ring_buffer_priv *buf;
};

static struct channel *chan;
static struct channel_priv channel_priv;
static const struct ring_buffer_config client_config;

/* Client callbacks */

static u64 client_ring_buffer_clock_read(struct channel *chan)
{
	return ring_buffer_clock_read(chan);
}

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

/**
 * ring_buffer_sb_header_size - called on buffer-switch to a new sub-buffer
 *
 * Return header size without padding after the structure. Don't use packed
 * structure because gcc generates inefficient code on some architectures
 * (powerpc, mips..)
 */
static size_t client_subbuffer_header_size(void)
{
	return offsetof(struct subbuffer_header, header_end);
}

/**
 * ring_buffer_write_trace_header - Write trace header
 * @trace: Trace information
 * @header: Memory address where the information must be written to
 */
static inline
void write_trace_header(void *priv, struct subbuffer_header *header)
{
	header->magic_number = 0x12345678;
	header->alignment = ring_buffer_get_alignment(&client_config);
	/* ... */
}

static void client_buffer_begin(struct ring_buffer *buf, u64 tsc,
			      unsigned int subbuf_idx)
{
	struct channel *chan = buf->backend.chan;
	struct subbuffer_header *header =
		(struct subbuffer_header *)
			ring_buffer_offset_address(&buf->backend,
				subbuf_idx * chan->backend.subbuf_size);

	header->cycle_count_begin = tsc;
	header->data_size = 0xFFFFFFFF; /* for debugging */
	write_trace_header(chan->backend.priv, header);
}

/*
 * offset is assumed to never be 0 here : never deliver a completely empty
 * subbuffer. data_size is between 1 and subbuf_size.
 */
static void client_buffer_end(struct ring_buffer *buf, u64 tsc,
			    unsigned int subbuf_idx, unsigned long data_size)
{
	struct channel *chan = buf->backend.chan;
	struct subbuffer_header *header =
		(struct subbuffer_header *)
			ring_buffer_offset_address(&buf->backend,
				subbuf_idx * chan->backend.subbuf_size);
	unsigned long records_lost = 0;

	header->data_size = data_size;
	header->subbuf_size = PAGE_ALIGN(data_size);
	header->cycle_count_end = tsc;
	records_lost += ring_buffer_get_records_lost_full(&client_config, buf);
	records_lost += ring_buffer_get_records_lost_wrap(&client_config, buf);
	records_lost += ring_buffer_get_records_lost_big(&client_config, buf);
	header->records_lost = records_lost;
}

static int client_buffer_create(struct ring_buffer *buf, void *priv,
				int cpu, const char *name)
{
	struct channel_priv *chan_priv = priv;
	struct ring_buffer_priv *buf_priv;
	char *tmpname;
	int ret = 0;

	if (client_config.alloc == RING_BUFFER_ALLOC_PER_CPU)
		buf_priv = per_cpu_ptr(chan_priv->buf, cpu);
	else
		buf_priv = chan_priv->buf;

	tmpname = kzalloc(NAME_MAX + 1, GFP_KERNEL);
	if (!tmpname) {
		ret = -ENOMEM;
		goto end;
	}

	snprintf(tmpname, NAME_MAX, "%s%s_%d",
		 (client_config.mode == RING_BUFFER_OVERWRITE) ? "flight-" : "",
		 name, cpu);

	buf_priv->dentry = debugfs_create_file(tmpname, S_IRUSR, NULL, buf,
					       &ring_buffer_file_operations);
	if (!buf_priv->dentry) {
		ret = -ENOMEM;
		goto free_name;
	}
free_name:
	kfree(tmpname);
end:
	return ret;
}

static void client_buffer_finalize(struct ring_buffer *buf, void *priv, int cpu)
{
	struct channel_priv *chan_priv = priv;
	struct ring_buffer_priv *buf_priv;

	if (client_config.alloc == RING_BUFFER_ALLOC_PER_CPU)
		buf_priv = per_cpu_ptr(chan_priv->buf, cpu);
	else
		buf_priv = chan_priv->buf;

	debugfs_remove(buf_priv->dentry);
}

static const struct ring_buffer_config client_config = {
	.cb.ring_buffer_clock_read = client_ring_buffer_clock_read,
	.cb.record_header_size = client_record_header_size,
	.cb.subbuffer_header_size = client_subbuffer_header_size,
	.cb.buffer_begin = client_buffer_begin,
	.cb.buffer_end = client_buffer_end,
	.cb.buffer_create = client_buffer_create,
	.cb.buffer_finalize = client_buffer_finalize,

	.tsc_bits = 64,
	.alloc = RING_BUFFER_ALLOC_PER_CPU,
	.sync = RING_BUFFER_SYNC_PER_CPU,
	.mode = RING_BUFFER_OVERWRITE,
	.align = RING_BUFFER_NATURAL,
	.backend = RING_BUFFER_PAGE,
	.output = RING_BUFFER_SPLICE,
	.oops = RING_BUFFER_OOPS_CONSISTENCY,
	.ipi = RING_BUFFER_IPI_BARRIER,
	.wakeup = RING_BUFFER_WAKEUP_BY_TIMER,
};

static void write_event_header(const struct ring_buffer_config *config,
			       struct ring_buffer_ctx *ctx)
{
	ring_buffer_write(config, ctx, &ctx->tsc, sizeof(ctx->tsc));
}

static noinline void trace_event(unsigned long data1, short data2)
{
	struct ring_buffer_ctx ctx;
	int ret, cpu;

	cpu = ring_buffer_get_cpu(&client_config);
	if (cpu < 0)
		return;
	ring_buffer_ctx_init(&ctx, chan, &channel_priv,
			     sizeof(struct payload),
			     sizeof(unsigned long),
			     cpu);

	ret = ring_buffer_reserve(&client_config, &ctx);
	if (ret)
		goto put;

	write_event_header(&client_config, &ctx);

	ring_buffer_align_ctx(&client_config, &ctx, sizeof(data1));
	ring_buffer_write(&client_config, &ctx, &data1, sizeof(data1));
	ring_buffer_align_ctx(&client_config, &ctx, sizeof(data2));
	ring_buffer_write(&client_config, &ctx, &data2, sizeof(data2));

	ring_buffer_commit(&client_config, &ctx);

put:
	ring_buffer_put_cpu(&client_config);
}

static noinline void use_ring_buffer(void)
{
	unsigned long i;

	for (i = 0; i < 1000000; i++)
		trace_event(i, (short)i);
}

static int __init ring_buffer_client_init(void)
{
	int ret;

	printk(KERN_DEBUG "Ring buffer client init begin\n");

	if (client_config.alloc == RING_BUFFER_ALLOC_PER_CPU)
		channel_priv.buf = alloc_percpu(struct ring_buffer_priv);
	else
		channel_priv.buf = kzalloc(sizeof(struct ring_buffer_priv),
					    GFP_KERNEL);
	if (!channel_priv.buf)
		return -ENOMEM;

	chan = channel_create(&client_config, "sample", &channel_priv, NULL,
			     1048576, 2,
			     100000, 1000);
	if (!chan) {
		ret = -EINVAL;
		goto error_create;
	}

	printk(KERN_DEBUG "Ring buffer client init end\n");

	use_ring_buffer();

	return 0;

error_create:
	if (client_config.alloc == RING_BUFFER_ALLOC_PER_CPU)
		free_percpu(channel_priv.buf);
	else
		kfree(channel_priv.buf);
	return ret;
}

static void __exit ring_buffer_client_exit(void)
{
	printk(KERN_DEBUG "Ring buffer client exit begin\n");
	channel_destroy(chan);
	if (client_config.alloc == RING_BUFFER_ALLOC_PER_CPU)
		free_percpu(channel_priv.buf);
	else
		kfree(channel_priv.buf);
	printk(KERN_DEBUG "Ring buffer client exit end\n");
}

module_init(ring_buffer_client_init);
module_exit(ring_buffer_client_exit);

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Mathieu Desnoyers");
MODULE_DESCRIPTION("Ring Buffer Client Template");
