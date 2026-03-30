// SPDX-License-Identifier: GPL-2.0-only
#include <linux/k22info.h>
#include <linux/syscalls.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/list.h>
#include <linux/threads.h>
#include <linux/errno.h>
#include <linux/pid.h>

static int count_processes(void)
{
	struct task_struct *p;
	int total = 1; /* account for init_task */

	read_lock(&tasklist_lock);
	for_each_process(p)
		total++;
	read_unlock(&tasklist_lock);

	return total;
}

/* get parent PROCESS because parent might be a non leader thread */
static struct task_struct *parent_process(struct task_struct *task)
{
	struct task_struct *parent = task->real_parent;

	if (!parent || parent == task)
		return NULL;

	return parent->group_leader;
}

/*
 * this might seem redundant but the tiebreaker is really needed
 * kthreadd and systemd have the exact same start_time
 * same goes for over 30 pairs of kworkers
 * use pid for tiebreak. it's clean and also traverses systemd first like in examples
 */
static bool child_before(struct task_struct *a, struct task_struct *b)
{
	if (a->start_time < b->start_time)
		return true;
	if (a->start_time > b->start_time)
		return false;
	return task_pid_nr(a) < task_pid_nr(b);
}

/*
 * we define child of a process = child of ANY thread in the process
 * since we iterate all threads, for a "dfs order" to be well defined,
 * we need a strict total order over all the independent thread-specific children lists
 * the order will be start_time ascending, just like the given examples
 * this means first child = oldest child of all threads
 * also crucial to note that any *->children list contains PROCESSES, not threads.
 * https://elixir.bootlin.com/linux/v6.14/source/kernel/fork.c#L2582
 */
static struct task_struct *first_child(struct task_struct *leader)
{
	struct task_struct *thread;
	struct task_struct *best = NULL;

	for_each_thread(leader, thread) {
		struct task_struct *child;

		list_for_each_entry(child, &thread->children, sibling) {
			if (!best || child_before(child, best))
				best = child;
		}
	}
	return best;
}

/*
 * next sibling based on our strict total order
 * aka oldest sibling that is also strictly younger than task
 */
static struct task_struct *next_sibling(struct task_struct *task)
{
	struct task_struct *parent, *thread, *candidate = NULL;

	parent = parent_process(task);
	if (!parent)
		return NULL;
	for_each_thread(parent, thread) {
		struct task_struct *child;

		list_for_each_entry(child, &thread->children, sibling) {
			if (task == child || child_before(child, task))
				continue;
			if (!candidate || child_before(child, candidate))
				candidate = child;
		}
	}
	return candidate;
}

static void fill_struct(struct k22info *info, struct task_struct *task,
	struct task_struct *child, struct task_struct *sibling)
{
	struct task_struct *parent;

	get_task_comm(info->comm, task);
	info->pid = task_pid_nr(task);
	info->nvcsw = task->nvcsw;
	info->nivcsw = task->nivcsw;
	info->start_time = (unsigned long)task->start_time;
	parent = parent_process(task);
	info->parent_pid = parent ? task_pid_nr(parent) : 0;
	info->first_child_pid = child ? task_pid_nr(child) : 0;
	info->next_sibling_pid = sibling ? task_pid_nr(sibling) : 0;
}

/*
 * second pass: iterative DFS without stack
 * essentially traverse only processes but account all threads to get
 * first child/next sibling and navigate.
 */
static void dfs_traverse_tasklist(struct k22info *kbuf, int buf_cap, int *observed, int *filled)
{
	struct task_struct *task = &init_task;
	*observed = 0;
	*filled   = 0;

	read_lock(&tasklist_lock);

	while (task) {
		struct task_struct *child, *sib;

		child = first_child(task);
		sib = next_sibling(task);
		(*observed)++; /* anything here is guaranteed to be a process */
		if (*filled < buf_cap) { /* continue without writing to get total processes */
			fill_struct(&kbuf[*filled], task, child, sib);
			(*filled)++;
		}
		if (child) {
			task = child;
			continue;
		}
		while (!sib) { /* no child or sibling: climb up until we find a sibling/hit root */
			task = parent_process(task);
			if (!task)
				goto out_unlock; /* we can just break, this is for clarity */
			sib = next_sibling(task);
		}
		task = sib;
	}
out_unlock:
	read_unlock(&tasklist_lock);
}

static long do_k22tree(struct k22info __user *buf, int __user *ne)
{
	int ret = 0;
	struct k22info *kbuf = NULL;
	int user_ne;
	int observed = 0;
	int filled = 0;
	int buf_cap = 0;
	int slack = 16; /* initial slack; we will double this with each retry */

	if (!buf || !ne) {
		ret = -EINVAL;
		goto out;
	}
	if (get_user(user_ne, ne)) {
		ret = -EFAULT;
		goto out;
	}
	if (user_ne <= 0) {
		ret = -EINVAL;
		goto out;
	}
	/* don't trust the user, allocate only what we need */
	observed = count_processes();

	/*
	 * we observed more than what we allocated but there's still space in user buffer -> retry
	 * this is safe; we will (eventually) either be capped by user_ne or fit all the processes
	 */
	while (observed > buf_cap && buf_cap < user_ne) {
		buf_cap = min(user_ne, observed + slack);

		kfree(kbuf); /* kfree(NULL) is safe, relax */
		kbuf = kcalloc(buf_cap, sizeof(*kbuf), GFP_KERNEL);
		if (!kbuf) {
			ret = -ENOMEM;
			goto out;
		}
		dfs_traverse_tasklist(kbuf, buf_cap, &observed, &filled);
		slack <<= 1;
	}
	/* write filled entries and their count in buf and ne, total processes as return value */
	if (filled > 0) {
		if (copy_to_user(buf, kbuf, filled * sizeof(struct k22info))) {
			ret = -EFAULT;
			goto out_free;
		}
	}
	if (put_user(filled, ne)) {
		ret = -EFAULT;
		goto out_free;
	}
	ret = observed;
out_free:
	kfree(kbuf);
out:
	return ret;
}

SYSCALL_DEFINE2(k22tree, struct k22info __user *, buf, int __user *, ne)
{
	return do_k22tree(buf, ne);
}
