#ifndef __MINOS_TASK_H__
#define __MINOS_TASK_H__

#include <minos/minos.h>
#include <minos/flag.h>
#include <config/config.h>
#include <minos/task_def.h>

extern struct task *os_task_table[OS_NR_TASKS];

struct task_desc {
	char *name;
	task_func_t func;
	void *arg;
	prio_t prio;
	uint16_t aff;
	unsigned long flags;
};

struct task_event {
	int id;
	struct task *task;
#define TASK_EVENT_EVENT_READY		0x0
#define TASK_EVENT_FLAG_READY		0x1
	int action;
	void *msg;
	uint32_t msk;
	uint32_t delay;
	flag_t flags;
};

#define DEFINE_TASK(nn, f, a, p, af, fl) \
	static const struct task_desc __used \
	task_desc_##f __section(.__task_desc) = { \
		.name = nn,		\
		.func = f,		\
		.arg = a,		\
		.prio = p,		\
		.aff = af,		\
		.flags = fl		\
	}

#define DEFINE_TASK_PERCPU(nn, f, a, fl) \
	static const struct task_desc __used \
	task_desc_##f __section(.__task_desc) = { \
		.name = nn,		\
		.func = f,		\
		.arg = a,		\
		.prio = OS_PRIO_PCPU,	\
		.aff = PCPU_AFF_PERCPU,	\
		.flags = fl		\
	}

#define DEFINE_REALTIME(nn, f, a, p, fl) \
	static const struct task_desc __used \
	task_desc_##f __section(.__task_desc) = { \
		.name = nn,		\
		.func = f,		\
		.arg = a,		\
		.prio = p,		\
		.aff = PCPU_AFF_NONE,	\
		.flags = fl		\
	}

static int inline is_idle_task(struct task *task)
{
	return (task->prio == OS_PRIO_IDLE);
}

static inline int get_task_pid(struct task *task)
{
	return task->pid;
}

static inline prio_t get_task_prio(struct task *task)
{
	return task->prio;
}

static inline int is_realtime_task(struct task *task)
{
	return (task->prio <= OS_LOWEST_PRIO);
}

static inline int is_percpu_task(struct task *task)
{
	return (task->prio == OS_PRIO_PCPU);
}

static inline int is_task_pending(struct task *task)
{
	return ((task->stat & TASK_STAT_PEND_ANY) != 
			TASK_STAT_RDY);
}

static inline int is_task_suspend(struct task *task)
{
	return !!(task->stat & TASK_STAT_SUSPEND);
}

static inline int is_task_ready(struct task *task)
{
	return ((task->stat == TASK_STAT_RDY) ||
			(task->stat == TASK_STAT_RUNNING));
}

int alloc_pid(prio_t prio, int cpuid);
void release_pid(int pid);
int task_ipi_event(struct task *task, struct task_event *ev, int wait);

int create_percpu_task(char *name, task_func_t func,
		void *arg, unsigned long flags);

int create_realtime_task(char *name, task_func_t func,
		void *arg, prio_t prio, unsigned long flags);

int create_vcpu_task(char *name, task_func_t func, void *arg,
		int aff, unsigned long flags);

int create_task(char *name, task_func_t func,
		void *arg, prio_t prio, uint16_t aff,
		unsigned long opt);

struct task_event *alloc_task_event(void);
void release_task_event(struct task_event *event);

#define task_lock(task)					\
	do {						\
		if (is_realtime_task(task))		\
			kernel_lock();			\
		else					\
			raw_spin_lock(&task->lock);	\
	} while (0)

#define task_unlock(task)				\
	do {						\
		if (is_realtime_task(task)) 		\
			kernel_unlock();		\
		else					\
			raw_spin_unlock(&task->lock);	\
	} while (0)

#define task_lock_irqsave(task, flags)			\
	do {						\
		if (is_realtime_task(task)) 		\
			kernel_lock_irqsave(flags);	\
		else					\
			spin_lock_irqsave(&task->lock, flags);	\
	} while (0)

#define task_unlock_irqrestore(task, flags)		\
	do {						\
		if (is_realtime_task(task)) 		\
			kernel_unlock_irqrestore(flags);\
		else					\
			spin_unlock_irqrestore(&task->lock, flags);	\
	} while (0)

#endif
