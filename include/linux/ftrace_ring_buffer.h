#ifndef _LINUX_FTRACE_RING_BUFFER_H
#define _LINUX_FTRACE_RING_BUFFER_H

#include <linux/kmemcheck.h>
#include <linux/mm.h>
#include <linux/seq_file.h>

struct ftrace_ring_buffer;
struct ftrace_ring_buffer_iter;

/*
 * Don't refer to this struct directly, use functions below.
 */
struct ftrace_ring_buffer_event {
	kmemcheck_bitfield_begin(bitfield);
	u32		type_len:5, time_delta:27;
	kmemcheck_bitfield_end(bitfield);

	u32		array[];
};

/**
 * enum ftrace_ring_buffer_type - internal ring buffer types
 *
 * @RINGBUF_TYPE_PADDING:	Left over page padding or discarded event
 *				 If time_delta is 0:
 *				  array is ignored
 *				  size is variable depending on how much
 *				  padding is needed
 *				 If time_delta is non zero:
 *				  array[0] holds the actual length
 *				  size = 4 + length (bytes)
 *
 * @RINGBUF_TYPE_TIME_EXTEND:	Extend the time delta
 *				 array[0] = time delta (28 .. 59)
 *				 size = 8 bytes
 *
 * @RINGBUF_TYPE_TIME_STAMP:	Sync time stamp with external clock
 *				 array[0]    = tv_nsec
 *				 array[1..2] = tv_sec
 *				 size = 16 bytes
 *
 * <= @RINGBUF_TYPE_DATA_TYPE_LEN_MAX:
 *				Data record
 *				 If type_len is zero:
 *				  array[0] holds the actual length
 *				  array[1..(length+3)/4] holds data
 *				  size = 4 + length (bytes)
 *				 else
 *				  length = type_len << 2
 *				  array[0..(length+3)/4-1] holds data
 *				  size = 4 + length (bytes)
 */
enum ftrace_ring_buffer_type {
	RINGBUF_TYPE_DATA_TYPE_LEN_MAX = 28,
	RINGBUF_TYPE_PADDING,
	RINGBUF_TYPE_TIME_EXTEND,
	/* FIXME: RINGBUF_TYPE_TIME_STAMP not implemented */
	RINGBUF_TYPE_TIME_STAMP,
};

unsigned ftrace_ring_buffer_event_length(struct ftrace_ring_buffer_event *event);
void *ftrace_ring_buffer_event_data(struct ftrace_ring_buffer_event *event);

/**
 * ftrace_ring_buffer_event_time_delta - return the delta timestamp of the event
 * @event: the event to get the delta timestamp of
 *
 * The delta timestamp is the 27 bit timestamp since the last event.
 */
static inline unsigned
ftrace_ring_buffer_event_time_delta(struct ftrace_ring_buffer_event *event)
{
	return event->time_delta;
}

/*
 * ftrace_ring_buffer_discard_commit will remove an event that has not
 *   ben committed yet. If this is used, then ftrace_ring_buffer_unlock_commit
 *   must not be called on the discarded event. This function
 *   will try to remove the event from the ring buffer completely
 *   if another event has not been written after it.
 *
 * Example use:
 *
 *  if (some_condition)
 *    ftrace_ring_buffer_discard_commit(buffer, event);
 *  else
 *    ftrace_ring_buffer_unlock_commit(buffer, event);
 */
void ftrace_ring_buffer_discard_commit(struct ftrace_ring_buffer *buffer,
				struct ftrace_ring_buffer_event *event);

/*
 * size is in bytes for each per CPU buffer.
 */
struct ftrace_ring_buffer *
__ftrace_ring_buffer_alloc(unsigned long size, unsigned flags, struct lock_class_key *key);

/*
 * Because the ring buffer is generic, if other users of the ring buffer get
 * traced by ftrace, it can produce lockdep warnings. We need to keep each
 * ring buffer's lock class separate.
 */
#define ftrace_ring_buffer_alloc(size, flags)			\
({							\
	static struct lock_class_key __key;		\
	__ftrace_ring_buffer_alloc((size), (flags), &__key);	\
})

void ftrace_ring_buffer_free(struct ftrace_ring_buffer *buffer);

int ftrace_ring_buffer_resize(struct ftrace_ring_buffer *buffer, unsigned long size);

struct ftrace_ring_buffer_event *ftrace_ring_buffer_lock_reserve(struct ftrace_ring_buffer *buffer,
						   unsigned long length);
int ftrace_ring_buffer_unlock_commit(struct ftrace_ring_buffer *buffer,
			      struct ftrace_ring_buffer_event *event);
int ftrace_ring_buffer_write(struct ftrace_ring_buffer *buffer,
		      unsigned long length, void *data);

struct ftrace_ring_buffer_event *
ftrace_ring_buffer_peek(struct ftrace_ring_buffer *buffer, int cpu, u64 *ts,
		 unsigned long *lost_events);
struct ftrace_ring_buffer_event *
ftrace_ring_buffer_consume(struct ftrace_ring_buffer *buffer, int cpu, u64 *ts,
		    unsigned long *lost_events);

struct ftrace_ring_buffer_iter *
ftrace_ring_buffer_read_prepare(struct ftrace_ring_buffer *buffer, int cpu);
void ftrace_ring_buffer_read_prepare_sync(void);
void ftrace_ring_buffer_read_start(struct ftrace_ring_buffer_iter *iter);
void ftrace_ring_buffer_read_finish(struct ftrace_ring_buffer_iter *iter);

struct ftrace_ring_buffer_event *
ftrace_ring_buffer_iter_peek(struct ftrace_ring_buffer_iter *iter, u64 *ts);
struct ftrace_ring_buffer_event *
ftrace_ring_buffer_read(struct ftrace_ring_buffer_iter *iter, u64 *ts);
void ftrace_ring_buffer_iter_reset(struct ftrace_ring_buffer_iter *iter);
int ftrace_ring_buffer_iter_empty(struct ftrace_ring_buffer_iter *iter);

unsigned long ftrace_ring_buffer_size(struct ftrace_ring_buffer *buffer);

void ftrace_ring_buffer_reset_cpu(struct ftrace_ring_buffer *buffer, int cpu);
void ftrace_ring_buffer_reset(struct ftrace_ring_buffer *buffer);

#ifdef CONFIG_FTRACE_RING_BUFFER_ALLOW_SWAP
int ftrace_ring_buffer_swap_cpu(struct ftrace_ring_buffer *buffer_a,
			 struct ftrace_ring_buffer *buffer_b, int cpu);
#else
static inline int
ftrace_ring_buffer_swap_cpu(struct ftrace_ring_buffer *buffer_a,
		     struct ftrace_ring_buffer *buffer_b, int cpu)
{
	return -ENODEV;
}
#endif

int ftrace_ring_buffer_empty(struct ftrace_ring_buffer *buffer);
int ftrace_ring_buffer_empty_cpu(struct ftrace_ring_buffer *buffer, int cpu);

void ftrace_ring_buffer_record_disable(struct ftrace_ring_buffer *buffer);
void ftrace_ring_buffer_record_enable(struct ftrace_ring_buffer *buffer);
void ftrace_ring_buffer_record_disable_cpu(struct ftrace_ring_buffer *buffer, int cpu);
void ftrace_ring_buffer_record_enable_cpu(struct ftrace_ring_buffer *buffer, int cpu);

unsigned long ftrace_ring_buffer_entries(struct ftrace_ring_buffer *buffer);
unsigned long ftrace_ring_buffer_overruns(struct ftrace_ring_buffer *buffer);
unsigned long ftrace_ring_buffer_entries_cpu(struct ftrace_ring_buffer *buffer, int cpu);
unsigned long ftrace_ring_buffer_overrun_cpu(struct ftrace_ring_buffer *buffer, int cpu);
unsigned long ftrace_ring_buffer_commit_overrun_cpu(struct ftrace_ring_buffer *buffer, int cpu);

u64 ftrace_ring_buffer_time_stamp(struct ftrace_ring_buffer *buffer, int cpu);
void ftrace_ring_buffer_normalize_time_stamp(struct ftrace_ring_buffer *buffer,
				      int cpu, u64 *ts);
void ftrace_ring_buffer_set_clock(struct ftrace_ring_buffer *buffer,
			   u64 (*clock)(void));

size_t ftrace_ring_buffer_page_len(void *page);


void *ftrace_ring_buffer_alloc_read_page(struct ftrace_ring_buffer *buffer);
void ftrace_ring_buffer_free_read_page(struct ftrace_ring_buffer *buffer, void *data);
int ftrace_ring_buffer_read_page(struct ftrace_ring_buffer *buffer, void **data_page,
			  size_t len, int cpu, int full);

struct trace_seq;

int ftrace_ring_buffer_print_entry_header(struct trace_seq *s);
int ftrace_ring_buffer_print_page_header(struct trace_seq *s);

enum ftrace_ring_buffer_flags {
	RB_FL_OVERWRITE		= 1 << 0,
};

#endif /* _LINUX_FTRACE_RING_BUFFER_H */
