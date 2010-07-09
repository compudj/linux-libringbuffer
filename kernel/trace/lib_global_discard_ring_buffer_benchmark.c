/*
 * ring buffer global discard library tester and benchmark
 *
 * Copyright (C) 2010 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#define RING_BUFFER_GLOBAL_TMPL
#define RING_BUFFER_DISCARD_TMPL
#define RING_BUFFER_NAME_TMPL "ring buffer global discard"
#include "ring_buffer_benchmark_template.h"

MODULE_AUTHOR("Mathieu Desnoyers");
MODULE_DESCRIPTION(RING_BUFFER_NAME_TMPL " test and benchmark");
MODULE_LICENSE("GPL");
