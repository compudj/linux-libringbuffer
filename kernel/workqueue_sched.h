/*
 * kernel/workqueue_sched.h
 *
 * Scheduler hooks for concurrency managed workqueue.  Only to be
 * included from sched.c and workqueue.c.
 */
static inline void wq_worker_waking_up(struct task_struct *task,
				       unsigned int cpu)
{
}

static inline struct task_struct *wq_worker_sleeping(struct task_struct *task,
						     unsigned int cpu)
{
	return NULL;
}
