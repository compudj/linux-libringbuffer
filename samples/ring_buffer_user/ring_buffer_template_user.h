#ifndef _RING_BUFFER_TEMPLATE_USER_H
#define _RING_BUFFER_TEMPLATE_USER_H

/*
 * ring_buffer_template_user.h
 *
 * Copyright (C) 2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring buffer template instance code example.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/types.h>
#include <linux/trace_clock.h>

/* Align data on its natural alignment */
#define RING_BUFFER_ALIGN

#include <linux/ringbuffer/config.h>

struct subbuffer_header {
	uint64_t cycle_count_begin;	/* Cycle count at subbuffer start */
	uint64_t cycle_count_end;	/* Cycle count at subbuffer end */
	uint32_t magic_number;		/*
					 * Trace magic number.
					 * contains endianness information.
					 */
	uint8_t major_version;
	uint8_t minor_version;
	uint8_t arch_size;		/* Architecture pointer size */
	uint8_t alignment;		/* ring buffer data alignment */
	uint64_t start_time_sec;	/* NTP-corrected start time */
	uint64_t start_time_usec;
	uint64_t start_freq;		/*
					 * Frequency at trace start,
					 * used all along the trace.
					 */
	uint32_t freq_scale;		/* Frequency scaling (divisor) */
	uint32_t data_size;		/* Size of data in subbuffer */
	uint32_t subbuf_size;		/* Subbuffer size (include padding) */
	uint32_t records_lost;		/*
					 * Records lost in this subbuffer since
					 * the beginning of the trace.
					 * (may overflow)
					 */
	uint8_t header_end[0];		/* End of header */
};

struct event_header {
	u64 tsc;
};

struct payload {
	unsigned long field1;
	short field2;
} RING_BUFFER_ALIGN_ATTR;

/*
 * Using the trace_clock_global() as an example.
 */
static inline notrace u64 ring_buffer_clock_read(struct channel *chan)
{
	return trace_clock_global();
}

/*
 * record_header_size - Calculate the header size and padding necessary.
 * @config: ring buffer instance configuration
 * @chan: channel
 * @offset: offset in the write buffer
 * @data_size: size of the payload
 * @pre_header_padding: padding to add before the header (output)
 * @rflags: reservation flags
 * @ctx: reservation context
 *
 * Returns the event header size (including padding).
 */
static inline notrace
unsigned char record_header_size(const struct ring_buffer_config *config,
				 struct channel *chan, size_t offset,
				 size_t data_size, size_t *pre_header_padding,
				 unsigned int rflags,
				 struct ring_buffer_ctx *ctx)
{
	size_t orig_offset = offset;
	size_t padding;

	padding = ring_buffer_align(config, offset,
				    sizeof(struct event_header));
	offset += padding;
	offset += sizeof(struct event_header);

	*pre_header_padding = padding;
	return offset - orig_offset;
}

#include <linux/ringbuffer/api.h>

#endif /* _RING_BUFFER_TEMPLATE_USER_H */
