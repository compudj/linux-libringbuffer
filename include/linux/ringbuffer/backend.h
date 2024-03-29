#ifndef _LINUX_RING_BUFFER_BACKEND_H
#define _LINUX_RING_BUFFER_BACKEND_H

/*
 * linux/ringbuffer/backend.h
 *
 * Copyright (C) 2008-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer backend (API).
 *
 * Dual LGPL v2.1/GPL v2 license.
 *
 * Credits to Steven Rostedt for proposing to use an extra-subbuffer owned by
 * the reader in flight recorder mode.
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/mm.h>

/* Internal helpers */
#include <linux/ringbuffer/backend_internal.h>
#include <linux/ringbuffer/frontend_internal.h>

/* Ring buffer backend API */

/* Ring buffer backend access (read/write) */

extern size_t ring_buffer_read(struct ring_buffer_backend *bufb,
			       size_t offset, void *dest, size_t len);

extern int __ring_buffer_copy_to_user(struct ring_buffer_backend *bufb,
				      size_t offset, void __user *dest,
				      size_t len);

extern int ring_buffer_read_cstr(struct ring_buffer_backend *bufb,
				 size_t offset, void *dest, size_t len);

extern struct page **
ring_buffer_read_get_page(struct ring_buffer_backend *bufb, size_t offset,
			  void ***virt);

/*
 * Return the address where a given offset is located.
 * Should be used to get the current subbuffer header pointer. Given we know
 * it's never on a page boundary, it's safe to write directly to this address,
 * as long as the write is never bigger than a page size.
 */
extern void *
ring_buffer_offset_address(struct ring_buffer_backend *bufb,
			   size_t offset);
extern void *
ring_buffer_read_offset_address(struct ring_buffer_backend *bufb,
				size_t offset);

/**
 * ring_buffer_write - write data to a buffer backend
 * @config : ring buffer instance configuration
 * @ctx: ring buffer context. (input arguments only)
 * @src : source pointer to copy from
 * @len : length of data to copy
 *
 * This function copies "len" bytes of data from a source pointer to a buffer
 * backend, at the current context offset. This is more or less a buffer
 * backend-specific memcpy() operation. Calls the slow path (_ring_buffer_write)
 * if copy is crossing a page boundary.
 */
static inline
void ring_buffer_write(const struct ring_buffer_config *config,
		       struct ring_buffer_ctx *ctx,
		       const void *src, size_t len)
{
	struct ring_buffer_backend *bufb = &ctx->buf->backend;
	struct channel_backend *chanb = &ctx->chan->backend;
	size_t sbidx, index;
	size_t offset = ctx->buf_offset;
	ssize_t pagecpy;
	struct ring_buffer_backend_pages *rpages;
	unsigned long sb_bindex, id;

	offset &= chanb->buf_size - 1;
	sbidx = offset >> chanb->subbuf_size_order;
	index = (offset & (chanb->subbuf_size - 1)) >> PAGE_SHIFT;
	pagecpy = min_t(size_t, len, (-offset) & ~PAGE_MASK);
	id = bufb->buf_wsb[sbidx].id;
	sb_bindex = subbuffer_id_get_index(config, id);
	rpages = bufb->array[sb_bindex];
	CHAN_WARN_ON(ctx->chan,
		     config->mode == RING_BUFFER_OVERWRITE
		     && subbuffer_id_is_noref(config, id));
	if (likely(pagecpy == len))
		ring_buffer_do_copy(config,
				    rpages->p[index].virt
				    + (offset & ~PAGE_MASK),
				    src, len);
	else
		_ring_buffer_write(bufb, offset, src, len, 0);
	ctx->buf_offset += len;
}

/*
 * This accessor counts the number of unread records in a buffer.
 * It only provides a consistent value if no reads not writes are performed
 * concurrently.
 */
static inline
unsigned long ring_buffer_get_records_unread(
				const struct ring_buffer_config *config,
				struct ring_buffer *buf)
{
	struct ring_buffer_backend *bufb = &buf->backend;
	struct ring_buffer_backend_pages *pages;
	unsigned long records_unread = 0, sb_bindex, id;
	unsigned int i;

	for (i = 0; i < bufb->chan->backend.num_subbuf; i++) {
		id = bufb->buf_wsb[i].id;
		sb_bindex = subbuffer_id_get_index(config, id);
		pages = bufb->array[sb_bindex];
		records_unread += v_read(config, &pages->records_unread);
	}
	if (config->mode == RING_BUFFER_OVERWRITE) {
		id = bufb->buf_rsb.id;
		sb_bindex = subbuffer_id_get_index(config, id);
		pages = bufb->array[sb_bindex];
		records_unread += v_read(config, &pages->records_unread);
	}
	return records_unread;
}

ssize_t ring_buffer_file_splice_read(struct file *in, loff_t *ppos,
				     struct pipe_inode_info *pipe, size_t len,
				     unsigned int flags);
loff_t ring_buffer_no_llseek(struct file *file, loff_t offset, int origin);

#endif /* _LINUX_RING_BUFFER_BACKEND_H */
