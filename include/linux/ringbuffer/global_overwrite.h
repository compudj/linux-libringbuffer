#ifndef _RING_BUFFER_GLOBAL_OVERWRITE_H
#define _RING_BUFFER_GLOBAL_OVERWRITE_H

/*
 * ring_buffer_global_overwrite.h
 *
 * Copyright (C) 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer global overwrite API. Overwrites oldest records when buffer is
 * full.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/ringbuffer/iterator.h>
#include <linux/types.h>

/*
 * ring_buffer_global_overwrite_create creates a global overwrite ring buffer.
 * buf_size is the buffer size, which will be rounded up to the next power of 2
 * (the floor value is 2*PAGE_SIZE). Returns the ring buffer channel address on
 * success, NULL on error.
 */
extern struct channel *ring_buffer_global_overwrite_create(size_t buf_size);

/*
 * ring_buffer_global_overwrite_destroy finalizes all channel's buffers, waits
 * for readers to release all references, and destroys the channel.
 */
extern void ring_buffer_global_overwrite_destroy(struct channel *chan);

/*
 * ring_buffer_global_overwrite_write writes a record into the ring buffer. The
 * record starts at the "src" address and is "len" bytes long. Returns 0 on
 * success, else it returns a negative error value.
 */
extern int ring_buffer_global_overwrite_write(struct channel *chan,
					      const void *src, size_t len);

#endif /* _RING_BUFFER_GLOBAL_OVERWRITE_H */
