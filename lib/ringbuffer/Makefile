ring_buffer-objs := ring_buffer_backend.o ring_buffer_frontend.o \
		ring_buffer_iterator.o ring_buffer_vfs.o \
		ring_buffer_splice.o ring_buffer_mmap.o

obj-$(CONFIG_LIB_RING_BUFFER) += ring_buffer.o

obj-$(CONFIG_LIB_RING_BUFFER_CLIENTS) += ring_buffer_global.o
obj-$(CONFIG_LIB_RING_BUFFER_CLIENTS) += ring_buffer_percpu.o
obj-$(CONFIG_LIB_RING_BUFFER_CLIENTS) += ring_buffer_percpu_local.o
