/*
 * ring_buffer_mmap.c
 *
 * Copyright (C) 2002-2005 - Tom Zanussi <zanussi@us.ibm.com>, IBM Corp
 * Copyright (C) 1999-2005 - Karim Yaghmour <karim@opersys.com>
 * Copyright (C) 2008-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Re-using content from kernel/relay.c.
 *
 * This file is released under the GPL v2.
 */

#include <linux/module.h>
#include <linux/mm.h>

#include <linux/ringbuffer/backend.h>
#include <linux/ringbuffer/frontend.h>
#include <linux/ringbuffer/vfs.h>

/*
 * fault() vm_op implementation for ring buffer file mapping.
 */
static int ring_buffer_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct ring_buffer *buf = vma->vm_private_data;
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;
	pgoff_t pgoff = vmf->pgoff;
	struct page **page;
	void **virt;
	unsigned long offset, sb_bindex;

	if (!buf)
		return VM_FAULT_OOM;

	/*
	 * Verify that faults are only done on the range of pages owned by the
	 * reader.
	 */
	offset = pgoff << PAGE_SHIFT;
	sb_bindex = subbuffer_id_get_index(config, buf->backend.buf_rsb.id);
	if (!(offset >= buf->backend.array[sb_bindex]->mmap_offset
	      && offset < buf->backend.array[sb_bindex]->mmap_offset +
			  buf->backend.chan->backend.subbuf_size))
		return VM_FAULT_SIGBUS;
	/*
	 * ring_buffer_read_get_page() gets the page in the current reader's
	 * pages.
	 */
	page = ring_buffer_read_get_page(&buf->backend, offset, &virt);
	if (!*page)
		return VM_FAULT_SIGBUS;
	get_page(*page);
	vmf->page = *page;

	return 0;
}

/*
 * vm_ops for relay file mappings.
 */
static const struct vm_operations_struct ring_buffer_mmap_ops = {
	.fault = ring_buffer_fault,
};

/**
 *	ring_buffer_mmap_buf: - mmap channel buffer to process address space
 *	@buf: ring buffer to map
 *	@vma: vm_area_struct describing memory to be mapped
 *
 *	Returns 0 if ok, negative on error
 *
 *	Caller should already have grabbed mmap_sem.
 */
static int ring_buffer_mmap_buf(struct ring_buffer *buf,
				struct vm_area_struct *vma)
{
	unsigned long length = vma->vm_end - vma->vm_start;
	struct channel *chan = buf->backend.chan;
	const struct ring_buffer_config *config = chan->backend.config;
	unsigned long mmap_buf_len;

	if (config->output != RING_BUFFER_MMAP)
		return -EINVAL;

	if (!buf)
		return -EBADF;

	mmap_buf_len = chan->backend.buf_size;
	if (chan->backend.extra_reader_sb)
		mmap_buf_len += chan->backend.subbuf_size;

	if (length != mmap_buf_len)
		return -EINVAL;

	vma->vm_ops = &ring_buffer_mmap_ops;
	vma->vm_flags |= VM_DONTEXPAND;
	vma->vm_private_data = buf;

	return 0;
}

/**
 *	relay_file_mmap - mmap file op for relay files
 *	@filp: the file
 *	@vma: the vma describing what to map
 *
 *	Calls upon relay_mmap_buf() to map the file into user space.
 */
int ring_buffer_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct ring_buffer *buf = filp->private_data;
	return ring_buffer_mmap_buf(buf, vma);
}
EXPORT_SYMBOL_GPL(ring_buffer_mmap);
