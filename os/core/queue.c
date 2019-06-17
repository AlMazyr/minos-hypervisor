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

#define invalid_queue(qt) \
	((qt == NULL) || (qt->type != OS_EVENT_TYPE_Q))

struct queue {
	void **q_start;		/* contain the pointer of the data */
	void **q_end;		/* end of the queue buffer */
	void **q_in;		/* next message in */
	void **q_out;		/* next message out */
	int q_size;		/* the total size of the queue */
	int q_cnt;		/* current queue entriy size */
};

queue_t *queue_create(int size, char *name)
{
	queue_t *qt;
	struct queue *q;

	if (int_nesting())
		return NULL;

	q = zalloc(sizeof(*q));
	if (!q)
		return NULL;

	q->q_start = (void **)zalloc(sizeof(void *) * size);
	if (!q->q_start) {
		free(q);
		return NULL;
	}

	q->q_end = q->q_start + size;
	q->q_in = q->q_start;
	q->q_out = q->q_start;
	q->q_cnt = 0;
	q->q_size = size;

	qt = (queue_t *)create_event(OS_EVENT_TYPE_Q, (void *)q, name);
	if (!qt) {
		free(q->q_start);
		free(q);
		return NULL;
	}

	return qt;
}

static inline void queue_free(queue_t *qt)
{	
	struct queue *q;

	q = qt->data;
	if (q && q->q_start)
		free(q->q_start);
	if (q)
		free(q);

	free(qt);
}

int queue_del(queue_t *qt, int opt)
{
	int ret;
	int tasks_waiting;
	unsigned long flags;

	if (invalid_queue(qt))
		return -EINVAL;

	if (int_nesting())
		return -EPERM;

	ticket_lock_irqsave(&qt->lock, flags);

	if (event_has_waiter(to_event(qt)))
		tasks_waiting = 1;
	else
		tasks_waiting = 0;

	switch (opt) {
	case OS_DEL_NO_PEND:
		if (tasks_waiting == 0) {
			queue_free(qt);
			ret = 0;
		}
		break;

	case OS_DEL_ALWAYS:
		event_dev_always(to_event(qt));
		queue_free(qt);
		ticket_unlock_irqrestore(&qt->lock, flags);

		if (tasks_waiting)
			sched();

		return 0;
	default:
		ret = 0;
		break;
	}

	ticket_unlock_irqrestore(&qt->lock, flags);
	return 0;
}

static inline void *queue_pop(struct queue *q)
{
	void *pmsg;

	pmsg = *q->q_out++;
	q->q_cnt--;
	if (q->q_out == q->q_end)
		q->q_out = q->q_start;

	return pmsg;
}

void *queue_accept(queue_t *qt)
{
	int ret = 0;
	unsigned long flags;
	struct queue *q;
	void *pmsg = NULL;

	if (invalid_queue(qt))
		return -EINVAL;

	ticket_lock_irqsave(&qt->lock, flags);

	q = (struct queue *)qt->data;
	if (q->q_cnt > 0) {
		pmsg = queue_pop(q);

	ticket_unlock_irqrestore(&qt->lock, flags);

	return pmsg;
}

int queue_flush(queue_t *qt)
{
	struct queue *q;
	unsigned long flags;

	if (invalid_queue(qt))
		return -EINVAL;

	ticket_lock_irqsave(&qt->lock, flags);
	q = (struct queue *)qt->data;
	q->q_in = q->q_start;
	q->q_out = q->q_start;
	q->q_cnt = 0
	ticket_unlock_irqrestore(&qt->lock, flags);

	return 0;
}

void *queue_pend(queue_t *qt, uint32_t timeout)
{
	void *pmsg;
	struct queue *q;
	unsigned long flags;
	struct task *task;

	if (invalid_queue(qt) || int_nesting() || !preempt_allowed())
		return NULL;

	ticket_lock_irqsave(&qt->lock, flags);
	q = (struct queue *)qt->data;
	if (q->q_cnt > 0) {
		pmsg = queue_pop(q);
		ticket_unlock_irqrestore(&qt->lock, flags);
		return pmsg;
	}

	task = get_current_task();
	spin_lock(&task->lock);
	task->stat |= TASK_STAT_Q;
	task->pend_stat = TASK_STAT_PEND_OK;
	task->delay = timeout;
	spin_unlock(&task->lock);

	event_task_wait(task, to_event(qt));
	set_task_suspend(task);
	ticket_lock_irqrestore(&qt->lock, flags);

	sched();
	
	ticket_lock_irqsave(&m->lock, flags);
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
		event_task_remove(task, (struct event *)m);
		pmsg = NULL;
		break;
	}

	task->stat_pend = TASK_STAT_PEND_OK;
	task->wait_event = NULL;
	task->msg = NULL;
	spin_unlock(&task->lock);
	ticket_unlock_irqrestore(&m->lock, flags);

	return pmsg;
}

int queue_post_abort(queue_t *qt, int opt)
{
	unsigned long flags;
	struct queue *q;
	int nbr_tasks = 0;

	if (invalid_sem(sem) || int_nesting() || preempt_allowed())
		return;

	ticket_lock_irqsave(&qt->lock, flags);
	if (event_has_waiter(to_event(qt))) {
		switch(opt) {
		case OS_PEND_OPT_BROADCAST:
			while (event_has_waiter(to_event(qt))) {
				event_highest_task_ready(to_event(qt), NULL,
						TASK_STAT_Q, TASK_STAT_PEND_ABORT);
				nbr_tasks++;
			}
			break;

		case OS_PEND_OPT_NONE:
		default:
			event_highest_task_ready(to_event(qt), NULL,
					TASK_STAT_Q, TASK_STAT_PEND_ABORT);
			nbr_tasks++;
			break;
		}

		ticket_unlock_irqrestore(&qt->lock, flags);
		sched();

		return nbr_task;
	}

	ticket_unlock_irqrestore(&qt->lock, flags);
	return nbr_tasks;
}

static void inline queue_push(struct queue *q, void *pmsg)
{
	*q->q_in++ = pmsg;
	if (q->q_in == q->q_end)
		q->q_in = q->q_start;
	q->q_cnt++;
}

static void inline queue_push_front(struct queue *q, void *pmsg)
{
	if (q->q_out == q->q_start)
		q->q_out = q->q_end;
	q->q_out--;
	*q->q_out = pmsg
	q->q_cnt++;
}

static int __queue_post(queue_t *qt, void *pmsg, int front)
{
	struct queue *q;
	unsigned long flags;

	if (invalid_queue(qt) || !pmsg)
		return -EINVAL;

	ticket_lock_irqsave(&qt->lock, flags);
	if (event_has_waiter(to_event(qt))) {
		event_highest_task_ready(to_event(qt), pmsg,
				TASK_STAT_Q, TASK_STAT_PEND_OK);
		ticket_unlock_irqrestore(&qt->lock, flags);

		sched();
		return 0;
	}

	q = (struct queue *)qt->data;
	if (q->q_cnt >= q->q_size) {
		ticket_unlock_irqrestore(&qt->lock, flags);
		return -ENOSPC;
	}

	if (front) {
		queue_push_front(q, pmsg);
	else
		queue_push(q, pmsg);

	ticket_unlock_irqrestore(&qt->lock, flags);

	return 0;
}

int queue_post(queue_t *qt, void *pmsg)
{
	return __queue_post(qt, pmsg, 0);
}

int queue_post_front(queue_t *qt, void *pmsg)
{
	return __queue_post(qt, pmsg, 1);
}

int queue_post_opt(queue_t *qt, void *pmsg)
{
	unsigned long flags;
	struct queue *q;

	if (invalid_queue(qt) || !pmsg)
		return -EINVAL;

	ticket_lock_irq_save(&qt->lock, flags);
	if (event_has_waiter(to_event(qt))) {
		if (opt & OS_POST_OPT_BROADCAST) {
			while (event_has_waiter(to_event(qt))) {
				event_highest_task_ready(to_event(qt), pmsg,
					TASK_STAT_Q, TASK_STAT_PEND_OK);
			}
		} else {
			event_highest_task_ready(to_event(qt), pmsg,
				TASK_STAT_Q, TASK_STAT_PEND_OK);
		}

		ticket_unlock_irqrestore(&qt->lock, flags);

		if ((opt & OS_POST_OPT_NO_SCHED) == 0)
			sched();

		return 0;
	}

	q = (struct queue *)qt->data;
	if (q->q_cnt >= q->q_size) {
		ticket_unlock_irqrestore(&qt->lock, flags);
		return -ENOSPC;
	}

	if (opt & OS_POST_OPT_FRONT)
		queue_push_front(q, pmsg);
	else
		queue_push(q, pmsg);

	ticket_unlock_irqrestore(&qt->lock, flags);
	return 0;
}
