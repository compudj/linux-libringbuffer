/*
 * ring buffer per-cpu overwrite library tester and benchmark
 *
 * Copyright (C) 2010 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#define RING_BUFFER_PER_CPU_TMPL
#define RING_BUFFER_OVERWRITE_TMPL
#define RING_BUFFER_NAME_TMPL "ring buffer per-cpu overwrite"
#include "ring_buffer_benchmark_template.h"

MODULE_AUTHOR("Mathieu Desnoyers");
MODULE_DESCRIPTION(RING_BUFFER_NAME_TMPL " test and benchmark");
MODULE_LICENSE("GPL");
