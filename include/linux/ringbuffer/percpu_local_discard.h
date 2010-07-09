#ifndef _RING_BUFFER_PERCPU_LOCAL_DISCARD_H
#define _RING_BUFFER_PERCPU_LOCAL_DISCARD_H

/*
 * ring_buffer_percpu_local_discard.h
 *
 * Copyright (C) 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Per-CPU local ring buffer discard API. Drops records when buffer is full.
 * Presents per-cpu-buffer iterators.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/ringbuffer/iterator.h>
#include <linux/types.h>

/*
 * ring_buffer_percpu_local_discard_create creates a per-cpu producer-consumer
 * ring buffer with local iterators. buf_size is the buffer size, which will be
 * rounded up to the next power of 2 (the floor value is 2*PAGE_SIZE). Returns
 * the ring buffer channel address on success, NULL on error.
 * on_buffer_create and on_buffer_finalize are callbacks called whenever a
 * per-cpu buffer is created or finalized. on_buffer_create returns 0 on
 * success.
 */
extern
struct channel *ring_buffer_percpu_local_discard_create(size_t buf_size,
		int (*on_buffer_create)(struct ring_buffer *buf, int cpu),
		void (*on_buffer_finalize)(struct ring_buffer *buf, int cpu));

/*
 * ring_buffer_percpu_local_discard_destroy finalizes all channel's buffers,
 * waits for readers to release all references, and destroys the channel.
 */
extern void ring_buffer_percpu_local_discard_destroy(struct channel *chan);

/*
 * ring_buffer_percpu_local_discard_write writes a record into the ring buffer.
 * The record starts at the "src" address and is "len" bytes long. Returns 0 on
 * success, else it returns a negative error value.
 */
extern
int ring_buffer_percpu_local_discard_write(struct channel *chan,
					   const void *src, size_t len);

#endif /* _RING_BUFFER_PERCPU_LOCAL_DISCARD_H */
