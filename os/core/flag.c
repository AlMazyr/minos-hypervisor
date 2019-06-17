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
#include <minos/flag.h>
#include <minos/task.h>
#include <minos/sched.h>

#define invalid_flag(f) \
	((f == NULL) || (f->type != OS_EVENT_TYPE_FLAG))

struct flag_grp *flag_create(flag_t flags)
{
	struct flag_grp *fg;

	if (int_nesting())
		return -EPERM;

	fg = zalloc(sizeof(*fg));
	if (fg)
		return NULL;

	fg->type = OS_EVENT_TYPE_FLAG;
	fg->flags = flags;
	init_list(&fg->wait_list);
	ticketlock_init(&fg->lock);

	return fg;
}

static inline flag_t flag_wait_set_all(struct flag_grp *grp,
		flag_t flags, int consume)
{
	flag_t flags_rdy;

	flags_rdy = grp->flags & flags;
	if (flags_rdy == flags) {
		if (consume)
			grp->flags &= ~flags_rdy;
	} else
		flags_rdy = 0;

	return flags_rdy;
}

static inline flag_t flag_wait_set_any(struct flag_grp *grp,
		flag_t flags, int consume)
{
	flag_t flags_rdy;

	flags_rdy = grp->flags & flags;
	if (flags_rdy != 0) {
		if (consume)
			grp->flags &= ~flags_rdy;
	} else
		flags_rdy = 0;
	
	return flags_rdy;
}

static inline flag_t flag_wait_clr_all(struct flag_grp *grp,
		flag_t flags, int consume)
{
	flag_t flags_rdy;

	flags_rdy = ~grp->flags & flags;
	if (flags_rdy == flags) {
		if (consume)
			grp->flags |= flags_rdy;
	} else
		flags_rdy = 0;

	return flags_rdy;
}

static inline flag_t flag_wait_clr_any(struct flag_grp *grp,
		flag_t flags, int consume)
{
	flag_t flags_rdy;

	flags_rdy = ~grp->flags & flags;
	if (flags_rdy != 0) {
		if (cosume)
			grp->flags |= flags_rdy;
	} else
		flags_rdy = 0;
	
	return flags_rdy;
}

flag_t flag_accept(struct flag_grp *grp, flag_t flags, int wait_type)
{
	unsigned long irq;
	flag_t flags_rdy;
	int result;
	int consume;

	if (invalid_flag(grp))
		return 0;

	result = wait_type & FLAG_COMSUME;
	if (result != 0) {
		wait_type &= ~FLAG_CONSUME;
		consume = 1;
	} else
		consume = 0;

	ticket_lock_irqsave(&grp->lock, irq);

	switch (wait_type) {
	case FLAG_WAIT_SET_ALL:
		flags_rdy = flag_wait_set_all(grp, flags, consume);
		break;
	case OS_FLAG_WAIT_SET_ANY:
		flags_rdy = flag_wait_set_any(grp, flags, consume);
		break;
	case OS_FLAG_WAIT_CLR_ALL:
		flags_rdy = flag_wait_clr_all(grp, flags, consume);
		break;
	case OS_FLAG_WAIT_CLR_ANY:
		flags_rdy = flag_wait_clr_any(grp, flags, consume);
		break;
	default:
		flags_rdy = 0;
		break;
	}

	ticket_unlock_irqrestore(&grp->lock, irq);
	return flag_rdy;
}

static int flag_task_ready(struct flag_node *node, flag_t flags)
{
	int sched;
	struct task *task;

	task = node->task;
	spin_lock(&task->lock);
	task->delay = 0;
	task->flags_rdy = flags;
	task->stat &= ~TASK_STAT_FLAG;
	task->pend_stat = TASK_STAT_PEND_OK;
	if (task->stat == TASK_STAT_RDY) {
		spin_unlock(&task->lock);
		set_task_ready(task);
		sched = 1;
	} else {
		sched = 0;
		spin_unlock(&task->lock)
	}

	return sched;
}

int flag_del(struct flag_grp *grp, int opt)
{
	int ret;
	int tasks_waiting;
	struct flag_node *pnode, *n;
	unsigned long irq;

	if (int_nesting() || invalid_flag(grp))
		return -EINVAL;

	ticket_lock_irqsave(&grp->lock, irq);

	if (!is_list_empty(&grp->wait_list))
		tasks_waiting = 1;
	else
		tasks_waiting = 0;

	switch (opt) {
	case OS_DEL_NO_PEND:
		if (tasks_waiting == 0) {
			free(grp);
			ret = 0;
		} else
			ret = -EPERM;
		break;
	case OS_DEL_ALWAYS:
		list_for_each_entry_safe(pnode, n,
				&grp->wait_list, list) {
			flag_task_ready(pnode, 0);
			list_del(&pnode->list);
		}

		free(grp);
		ticket_unlock_irqrestore(irq);

		if (tasks_waiting)
			sched();
		return 0;
	default:
		ret = -EINVAL;
		break;
	}

	ticket_unlock_irqrestore(&grp->lock, irq);
	return ret;
}

static void flag_block(struct flag_grp *grp, struct flag_node *pnode,
		flag_t flags, int wait_type, uint32_t timeout)
{
	struct task *task = get_current_task;

	memset(pnode, 0, sizeof(*pnode));
	pnode->flags = flags;
	pnode->wait_type = wait_type;
	pnode->task = task;
	pnode->flags_grp = grp;

	spin_lock(&task->lock);
	task->stat |= TASK_STAT_FLAG;
	task->pend_stat = TASK_STAT_PEND_OK;
	task->delay = timeout;
	task->flag_node = pnode;
	list_add_tail(&grp->wait_list, &pnode->list);
	spin_unlock(&task->lock);

	set_task_suspend(task);
}

flag_t flag_pend(struct flag_grp *grp, flag_t flags,
		int wait_type, uint32_t timeout)
{
	unsigned long irq;
	struct flag_node node;
	flag_t flags_rdy;
	int result, consume;
	int pend_stat;
	struct task *task = get_current_task;

	if (invalid_flag(grp) || int_nesting() || !preempt_allowed())
		return 0;

	result = wait_type & FLAG_CONSUME;
	if (result) {
		wait_type &= ~FLAG_CONSUME;
		consume = 1;
	} else
		consume = 0;

	ticket_lock_irqsave(&grp->lock, irq);

	switch (wait_type) {
	case FLAG_WAIT_SET_ALL:
		flags_rdy = flag_wait_set_all(grp, flags. consume);
		break;
	case FLAG_WAIT_SET_ANY:
		flags_rdy = flag_wait_set_any(grp, flags. consume);
		break;
	case FLAG_WAIT_CLR_ALL:
		flags_rdy = flag_wait_clr_all(grp, flags, consume);
		break;
	case FLAG_WAIT_CLR_ANY:
		flags_rdy = flag_wait_clr_any(grp, flags, consume);
		break;
	default:
		flags_rdy = 0;
		break;
	}

	if (flags_rdy) {
		ticket_unlock_irqrestore(&grp->lock, irq);
		return flags_rdy;
	}

	flag_block(grp, &node, flags, wait_type, timeout);
	ticket_unlock_irqrestore(&grp->lock, irq);

	sched();

	ticket_lock_irqsave(&grp->lock, irq);
	spin_lock(&task->lock);

	if (task->stat_pend != TASK_STAT_PEND_OK) {
		pend_stat = task->stat_pend;
		task->stat_pend = TASK_STAT_PEND_OK;
		list_del(&node.list);
		flags_rdy = 0;
	} else {
		flags_rdy = task->flags_rdy;
		if (consume) {
			switch (wait_type) {
			case FLAG_WAIT_SET_ALL:
			case FLAG_WAIT_SET_ANY:
				grp->flags &= ~flags_rdy;
				break;

			case FLAG_WAIT_CLR_ALL:
			case FLAG_WAIT_CLR_ANY:
				grp->flags |= flags_rdy;
				break;

			default:
				flags_rdy = 0;
				break;
			}
		}
	}

	spin_unlock(&task->lock);
	ticket_unlock_irqrestore(&grp->lock, irq);

	return flags_rdy;
}

flag_t flag_pend_get_flags_ready(void)
{
	struct task *task = get_current_task();
	unsigned long flags;
	flag_t flags;

	spin_lock_irqsave(&task->lock, flags);
	flags = task->flags_rdy;
	spin_unlock_irqrestore(&task->lock, flags);

	return flags;
}

flag_t flag_post(struct flag_grp *grp, flag_t flags, int opt)
{
	int sched;
	flag_t flags_rdy;
	unsigned long irq;
	struct flag_node *pnode, *n;

	if (invalid_flag(grp) || (opt > FLAG_SET))
		return -EINVAL;

	ticket_lock_irqsave(&grp->lock, irq);
	switch (opt) {
	case FLAG_CLR:
		grp->flags &= ~flags;
		break;
	
	case FLAG_SET:
		grp->flag |= flags;
		break;
	}

	sched = 0;
	list_for_each_entry_safe(pnode, n, &grp->wait_list, list) {
		switch (pnode->wait_type) {
		case FLAG_WAIT_SET_ALL:
			flags_rdy = grp->flags & pnode->flags;
			if (flags_rdy == pnode->flags) {
				sched = flag_task_ready(pnode,
						flags_rdy);
			}
			break;

		case FLAG_WAIT_SET_ANY:
			flags_rdy = grp->flags & pnode->flags;
			if (flags_rdy != 0) {
				sched = flag_task_ready(pnode,
						flags_rdy);
			}
			break;

		case FLAG_WAIT_CLR_ALL:
			flags_rdy = ~grp->flags & pnode->flags;
			if (flags_rdy == pnode->flags) {
				sched = flag_task_ready(pnode,
						flags_rdy);
			}
			break;

		case FLAG_WAIT_CLR_ANY:
			flags_rdy = ~grp->flags & pnode->flags;
			if (flags_rdy != 0) {
				sched = flag_task_ready(pnode,
						flags_rdy);
			}

		default:
			ticket_unlock_irqrestore(&grp->lock, irq);
			return 0;
		}
	}

	ticket_unlock_irqrestore(&grp->lock, irq);

	if (sched)
		sched();

	ticket_lock_irqsave(&grp->lock, irq);
	flags_rdy = grp->flags;
	ticket_lock_irqrestore(&grp->lock, irq);

	return flags_rdy;
}
