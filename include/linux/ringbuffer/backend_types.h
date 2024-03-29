#ifndef _LINUX_RING_BUFFER_BACKEND_TYPES_H
#define _LINUX_RING_BUFFER_BACKEND_TYPES_H

/*
 * linux/ringbuffer/backend_types.h
 *
 * Copyright (C) 2008-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer backend (types).
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/cpumask.h>
#include <linux/types.h>

struct ring_buffer_backend_page {
	void *virt;			/* page virtual address (cached) */
	struct page *page;		/* pointer to page structure */
};

struct ring_buffer_backend_pages {
	unsigned long mmap_offset;	/* offset of the subbuffer in mmap */
	union v_atomic records_commit;	/* current records committed count */
	union v_atomic records_unread;	/* records to read */
	unsigned long data_size;	/* Amount of data to read from subbuf */
	struct ring_buffer_backend_page p[];
};

struct ring_buffer_backend_subbuffer {
	/* Identifier for subbuf backend pages. Exchanged atomically. */
	unsigned long id;		/* backend subbuffer identifier */
};

/*
 * Forward declaration of frontend-specific channel and ring_buffer.
 */
struct channel;
struct ring_buffer;

struct ring_buffer_backend {
	/* Array of ring_buffer_backend_subbuffer for writer */
	struct ring_buffer_backend_subbuffer *buf_wsb;
	/* ring_buffer_backend_subbuffer for reader */
	struct ring_buffer_backend_subbuffer buf_rsb;
	/*
	 * Pointer array of backend pages, for whole buffer.
	 * Indexed by ring_buffer_backend_subbuffer identifier (id) index.
	 */
	struct ring_buffer_backend_pages **array;
	unsigned int num_pages_per_subbuf;

	struct channel *chan;		/* Associated channel */
	int cpu;			/* This buffer's cpu. -1 if global. */
	union v_atomic records_read;	/* Number of records read */
	unsigned int allocated:1;	/* Bool: is buffer allocated ? */
};

struct channel_backend {
	unsigned long buf_size;		/* Size of the buffer */
	unsigned long subbuf_size;	/* Sub-buffer size */
	unsigned int subbuf_size_order;	/* Order of sub-buffer size */
	unsigned int num_subbuf_order;	/*
					 * Order of number of sub-buffers/buffer
					 * for writer.
					 */
	unsigned int buf_size_order;	/* Order of buffer size */
	int extra_reader_sb:1;		/* Bool: has extra reader subbuffer */
	struct ring_buffer *buf;	/* Channel per-cpu buffers */

	unsigned long num_subbuf;	/* Number of sub-buffers for writer */
	u64 start_tsc;			/* Channel creation TSC value */
	void *priv;			/* Client-specific information */
	struct notifier_block cpu_hp_notifier;	 /* CPU hotplug notifier */
	const struct ring_buffer_config *config; /* Ring buffer configuration */
	cpumask_var_t cpumask;		/* Allocated per-cpu buffers cpumask */
	char name[NAME_MAX];		/* Channel name */
};

#endif /* _LINUX_RING_BUFFER_BACKEND_TYPES_H */
