#ifndef _LINUX_RING_BUFFER_ITERATOR_H
#define _LINUX_RING_BUFFER_ITERATOR_H

/*
 * linux/ringbuffer/iterator.h
 *
 * (C) Copyright 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer and channel iterators.
 *
 * Author:
 *	Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/ringbuffer/backend.h>
#include <linux/ringbuffer/frontend.h>

/*
 * ring_buffer_get_next_record advances the buffer read position to the next
 * record. It returns either the size of the next record, -EAGAIN if there is
 * currently no data available, or -ENODATA if no data is available and buffer
 * is finalized.
 */
extern ssize_t ring_buffer_get_next_record(struct channel *chan,
					   struct ring_buffer *buf);

/*
 * channel_get_next_record advances the buffer read position to the next record.
 * It returns either the size of the next record, -EAGAIN if there is currently
 * no data available, or -ENODATA if no data is available and buffer is
 * finalized.
 * Returns the current buffer in ret_buf.
 */
extern ssize_t channel_get_next_record(struct channel *chan,
				       struct ring_buffer **ret_buf);

/**
 * read_current_record - copy the buffer current record into dest.
 * @buf: ring buffer
 * @dest: destination where the record should be copied
 *
 * dest should be large enough to contain the record. Returns the number of
 * bytes copied.
 */
static inline size_t read_current_record(struct ring_buffer *buf, void *dest)
{
	return ring_buffer_read(&buf->backend, buf->iter.read_offset,
				dest, buf->iter.payload_len);
}

extern int ring_buffer_iterator_open(struct ring_buffer *buf);
extern void ring_buffer_iterator_release(struct ring_buffer *buf);
extern int channel_iterator_open(struct channel *chan);
extern void channel_iterator_release(struct channel *chan);

extern const struct file_operations channel_payload_file_operations;
extern const struct file_operations ring_buffer_payload_file_operations;

/*
 * Used internally.
 */
int channel_iterator_init(struct channel *chan);
void channel_iterator_unregister_notifiers(struct channel *chan);
void channel_iterator_free(struct channel *chan);
void channel_iterator_reset(struct channel *chan);
void ring_buffer_iterator_reset(struct ring_buffer *buf);

#endif /* _LINUX_RING_BUFFER_ITERATOR_H */
