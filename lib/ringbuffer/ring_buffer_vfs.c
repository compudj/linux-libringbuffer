/*
 * ring_buffer_vfs.c
 *
 * Copyright (C) 2009-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Ring Buffer VFS file operations.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/module.h>
#include <linux/fs.h>

#include <linux/ringbuffer/backend.h>
#include <linux/ringbuffer/frontend.h>
#include <linux/ringbuffer/vfs.h>

/**
 *	ring_buffer_open - ring buffer open file operation
 *	@inode: opened inode
 *	@file: opened file
 *
 *	Open implementation. Makes sure only one open instance of a buffer is
 *	done at a given moment.
 */
int ring_buffer_open(struct inode *inode, struct file *file)
{
	struct ring_buffer *buf = inode->i_private;
	int ret;

	ret = ring_buffer_open_read(buf);
	if (ret)
		return ret;

	file->private_data = buf;
	ret = nonseekable_open(inode, file);
	if (ret)
		goto release_read;
	return 0;

release_read:
	ring_buffer_release_read(buf);
	return ret;
}

/**
 *	ring_buffer_release - ring buffer release file operation
 *	@inode: opened inode
 *	@file: opened file
 *
 *	Release implementation.
 */
int ring_buffer_release(struct inode *inode, struct file *file)
{
	struct ring_buffer *buf = inode->i_private;

	ring_buffer_release_read(buf);

	return 0;
}

/**
 *	ring_buffer_poll - ring buffer poll file operation
 *	@filp: the file
 *	@wait: poll table
 *
 *	Poll implementation.
 */
unsigned int ring_buffer_poll(struct file *filp, poll_table *wait)
{
	unsigned int mask = 0;
	struct inode *inode = filp->f_dentry->d_inode;
	struct ring_buffer *buf = inode->i_private;
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;
	int finalized;

	if (filp->f_mode & FMODE_READ) {
		poll_wait_set_exclusive(wait);
		poll_wait(filp, &buf->read_wait, wait);

		finalized = ring_buffer_is_finalized(config, buf);
		/*
		 * ring_buffer_is_finalized() contains a smp_rmb() ordering
		 * finalized load before offsets loads.
		 */

		WARN_ON(atomic_long_read(&buf->active_readers) != 1);
retry:
		if (subbuf_trunc(ring_buffer_get_offset(config, buf), chan)
		  - subbuf_trunc(ring_buffer_get_consumed(config, buf), chan)
		  == 0) {
			if (finalized)
				return POLLHUP;
			else {
				/*
				 * The memory barriers
				 * __wait_event()/wake_up_interruptible() take
				 * care of "raw_spin_is_locked" memory ordering.
				 */
				if (raw_spin_is_locked(&buf->raw_idle_spinlock))
					goto retry;
				else
					return 0;
			}
		} else {
			if (subbuf_trunc(ring_buffer_get_offset(config, buf),
					 chan)
			  - subbuf_trunc(ring_buffer_get_consumed(config, buf),
					 chan)
			  >= chan->backend.buf_size)
				return POLLPRI | POLLRDBAND;
			else
				return POLLIN | POLLRDNORM;
		}
	}
	return mask;
}

/**
 *	ring_buffer_ioctl - control ring buffer reader synchronization
 *
 *	@inode: the inode
 *	@filp: the file
 *	@cmd: the command
 *	@arg: command arg
 *
 *	This ioctl implements commands necessary for producer/consumer
 *	and flight recorder reader interaction :
 *	RING_BUFFER_GET_SUBBUF
 *		Get the next sub-buffer that can be read. It never blocks.
 *	RING_BUFFER_PUT_SUBBUF
 *		Release the currently read sub-buffer. Parameter is the last
 *		put subbuffer (returned by GET_SUBBUF).
 *	RING_BUFFER_GET_SUBBUF_SIZE
 *		returns the size of the current sub-buffer.
 *	RING_BUFFER_GET_MAX_SUBBUF_SIZE
 *		returns the maximum size for sub-buffers.
 *	RING_BUFFER_GET_NUM_SUBBUF
 *		returns the number of reader-visible sub-buffers in the per cpu
 *              channel (for mmap).
 *      RING_BUFFER_GET_MMAP_READ_OFFSET
 *              returns the offset of the subbuffer belonging to the reader.
 *              Should only be used for mmap clients.
 */
int ring_buffer_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
		      unsigned long arg)
{
	struct ring_buffer *buf = inode->i_private;
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;
	u32 __user *argp = (u32 __user *)arg;

	switch (cmd) {
	case RING_BUFFER_GET_SUBBUF:
	{
		unsigned long consumed;
		int ret;

		ret = ring_buffer_get_subbuf(buf, &consumed);
		if (ret)
			return ret;
		else
			return put_user((u32)consumed, argp);
		break;
	}
	case RING_BUFFER_PUT_SUBBUF:
	{
		u32 uconsumed_old;
		int ret;
		long consumed_old;

		ret = get_user(uconsumed_old, argp);
		if (ret)
			return ret; /* will return -EFAULT */

		consumed_old = ring_buffer_get_consumed(config, buf);
		consumed_old = consumed_old & (~0xFFFFFFFFL);
		consumed_old = consumed_old | uconsumed_old;
		ring_buffer_put_subbuf(buf, consumed_old);
		break;
	}
	case RING_BUFFER_GET_SUBBUF_SIZE:
		return put_user(ring_buffer_get_read_data_size(config, buf),
				argp);
		break;
	case RING_BUFFER_GET_MAX_SUBBUF_SIZE:
		return put_user((u32)chan->backend.subbuf_size, argp);
		break;
	/*
	 * TODO: mmap length is currently limited to 4GB, even on 64-bit
	 * architectures. We should be more clever in dealing with ioctl
	 * compatibility here. Using a u32 is probably not what we want.
	 */
	case RING_BUFFER_GET_MMAP_LEN:
	{
		unsigned long mmap_buf_len;

		if (config->output != RING_BUFFER_MMAP)
			return -EINVAL;
		mmap_buf_len = chan->backend.buf_size;
		if (chan->backend.extra_reader_sb)
			mmap_buf_len += chan->backend.subbuf_size;
		if (mmap_buf_len > INT_MAX)
			return -EFBIG;
		return put_user((u32)mmap_buf_len, argp);
		break;
	}
	case RING_BUFFER_GET_MMAP_READ_OFFSET:
	{
		unsigned long sb_bindex;

		if (config->output != RING_BUFFER_MMAP)
			return -EINVAL;
		sb_bindex = subbuffer_id_get_index(config,
						  buf->backend.buf_rsb.id);
		return put_user((u32)buf->backend.array[sb_bindex]->mmap_offset,
				 argp);
		break;
	}
	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

#ifdef CONFIG_COMPAT
long ring_buffer_compat_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	long ret = -ENOIOCTLCMD;

	lock_kernel();
	ret = ring_buffer_ioctl(file->f_dentry->d_inode, file, cmd, arg);
	unlock_kernel();

	return ret;
}
#endif

const struct file_operations ring_buffer_file_operations = {
	.open = ring_buffer_open,
	.release = ring_buffer_release,
	.poll = ring_buffer_poll,
	.splice_read = ring_buffer_splice_read,
	.mmap = ring_buffer_mmap,
	.ioctl = ring_buffer_ioctl,
	.llseek = ring_buffer_no_llseek,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ring_buffer_compat_ioctl,
#endif
};
EXPORT_SYMBOL_GPL(ring_buffer_file_operations);

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Mathieu Desnoyers");
MODULE_DESCRIPTION("Ring Buffer Library VFS");
