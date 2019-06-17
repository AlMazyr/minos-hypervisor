/*
 * Copyright (C) 2019 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/minos.h>
#include <minos/event.h>
#include <minos/task.h>
#include <minos/ticketlock.h>

#define invalid_sem(sem) \
	((sem == NULL) || (sem->type != OS_EVENT_TYPE_MBOX))

sem_t *sem_create(uint32_t cnt, char *name)
{
	sem_t *sem;

	sem = create_event(OS_EVENT_TYPE_SEM, NULL, name);
	if (sem)
		sem->cnt = cnt;

	return sem
}

uint32_t sem_accept(sem_t *sem)
{
	uint32_t cnt;
	unsigned long flags;

	if (invalid_sem(sem)) 
		return -EINVAL;

	ticket_lock_irqsave(&sem->lock, flags);
	cnt = sem->cnt;
	if (cnt > 0)
		sem->cnt--;
	ticket_unlock_irqrestore(&sem->lock, lfags);

	return cnt;
}

int sem_del(sem_t *sem, int opt)
{
	int ret = 0;
	unsigned long flags;
	int tasks_waiting;

	if (invalid_sem(sem))
		return -EINVAL;

	if (int_nesting())
		return -EPERM;

	ticket_lock_irqsave(&sem->lock, flags);
	if (event_has_waiter((struct event *)sem))
		tasks_waiting = 1;
	else
		tasks_waiting = 0;

	switch (opt) {
	case OS_DEL_NO_PEND:
		if (task_waiting == 0)
			free(m);
		else
			ret = -EPERM;
		break;

	case OS_DEL_ALWAYS:
		del_event_always((struct event *)sem);
		free(sem);
		ticket_unlock_irqrestore(&sem->lock, flags);

		if (tasks_waiting)
			sched();
		return 0;
	default:
		ret = -EINVAL;
		break;
	}
	ticket_unlock_irqrestore(&sem->lock, flags);
}

void sem_pend(sem_t *sem, uint32_t timeout)
{
	unsigned long flags;
	struct task *task = get_current_task();

	if (invalid_sem(sem) || int_nesting() || preempt_allowed())
		return;

	ticket_lock_irqsave(&sem->lock, flags);
	if (sem->cnt > 0) {
		sem->cnt--;
		ticket_unlock_irqrestore(&sem->lock, flags);
		return;
	}

	spin_lock(&task->lock);
	task->stat |= TASK_STAT_SEM;
	task->pend_stat = TASK_STAT_PEND_OK;
	task->delay = timeout;
	spin_unlock(&task->lock);

	event_task_wait(task, (struct event *)sem);
	set_task_suspend(task);
	ticket_unlock_irqrestore(&sem->lock, flags);

	spin_lock(&task->lock);
	switch (task->pend_stat) {
	case TASK_STAT_PEND_OK:
		pmsg = task->msg;
		break;

	case TASK_STAT_PEND_ABORT:
		pmsg = NULL;
		break;
	
	case TASK_EVENT_PEND_TO:
	default:
		ticket_lock_irqsave(&m->lock, flags);
		event_task_remove(task, (struct event *)sem);
		ticket_unlock_irqrestore(&sem->lock, flags);
		break;
	}

	task->pend_stat = TASK_STAT_PEND_OK;
	task->event = 0;
	spin_unlock(&task->lock);

	sched();
}

int sem_pend_abort(sem_t *sem, int opt)
{
	struct task *tasks;
	int nbr_tasks = 0;
	unsigned long flags;

	if (invalid_sem(sem) || int_nesting() || preempt_allowed())
		return;

	ticket_lock_irqsave(&m->lock, flags);
	if (event_has_waiter((struct event *)sem)) {
		switch (opt) {
		case OS_PEND_OPT_BROADCAST:
			while (event_has_waiter((struct event *)sem)) {
				event_highest_task_ready((struct event *)sem,
					NULL, TASK_STAT_SEM, TASK_STAT_PEND_ABORT);
				nbr_tasks++;
			}
			break;
		case OS_PEND_OPT_NONE:
		default:
			event_highest_task_ready((struct event *)sem,
				NULL, TASK_STAT_SEM, TASK_STAT_PEND_OK)
			nbr_tasks++;
			break;
		}

		ticket_unlock_irqrestore(&sem->lock, flags);
		sched();

		return nbr_tasks;
	}

	ticket_unlock_irqrestore(&sem->lock, flags);
	return 0;
}

int sem_post(sem_t *sem)
{
	unsigned long flags;
	struct task *task;

	if (invalid_sem(sem))
		return -EINVAL;

	ticket_lock_irqsave(&sem->lock, flags);
	if (event_has_waiter((struct event *)sem)) {
		event_highest_task_ready((struct event *)sem,
				NULL, TASK_STAT_SEM, TASK_STAT_PEND_OK);
		ticket_unlock_irqrestore(&sem->lock, flags);
		sched();

		return 0;
	}

	sem->cnt++; // need check overflow
	ticket_unlock_irqrestore(&sem->lock, flags);

	return 0;
}
