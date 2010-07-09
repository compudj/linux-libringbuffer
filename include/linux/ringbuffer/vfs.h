#ifndef _LINUX_RING_BUFFER_VFS_H
#define _LINUX_RING_BUFFER_VFS_H

/*
 * linux/ringbuffer/vfs.h
 *
 * (C) Copyright 2005-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Wait-free ring buffer VFS file operations.
 *
 * Author:
 *	Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/fs.h>
#include <linux/poll.h>

/* VFS API */

extern const struct file_operations ring_buffer_file_operations;

/*
 * Internal file operations.
 */

int ring_buffer_open(struct inode *inode, struct file *file);
int ring_buffer_release(struct inode *inode, struct file *file);
unsigned int ring_buffer_poll(struct file *filp, poll_table *wait);
ssize_t ring_buffer_splice_read(struct file *in, loff_t *ppos,
				struct pipe_inode_info *pipe, size_t len,
				unsigned int flags);
int ring_buffer_mmap(struct file *filp, struct vm_area_struct *vma);

/* Ring Buffer ioctl() and ioctl numbers */
int ring_buffer_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
		      unsigned long arg);
#ifdef CONFIG_COMPAT
long ring_buffer_compat_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg);
#endif

/* Get the next sub-buffer that can be read. */
#define RING_BUFFER_GET_SUBBUF			_IOR(0xF6, 0x00, __u32)
/* Release the oldest reserved (by "get") sub-buffer. */
#define RING_BUFFER_PUT_SUBBUF			_IOW(0xF6, 0x01, __u32)
/* returns the size of the current sub-buffer. */
#define RING_BUFFER_GET_SUBBUF_SIZE		_IOR(0xF6, 0x02, __u32)
/* returns the maximum size for sub-buffers. */
#define RING_BUFFER_GET_MAX_SUBBUF_SIZE		_IOR(0xF6, 0x03, __u32)
/* returns the length to mmap. */
#define RING_BUFFER_GET_MMAP_LEN		_IOR(0xF6, 0x04, __u32)
/* returns the offset of the subbuffer belonging to the mmap reader. */
#define RING_BUFFER_GET_MMAP_READ_OFFSET	_IOR(0xF6, 0x05, __u32)

#endif /* _LINUX_RING_BUFFER_VFS_H */
