// SPDX-License-Identifier: GPL-2.0-only
#include "sched.h"

void init_grr_rq(struct grr_rq *grr_rq)
{
	INIT_LIST_HEAD(&grr_rq->queue);
	grr_rq->nr_running = 0;
	grr_rq->next_balance = jiffies + msecs_to_jiffies(500);
}

static inline struct task_struct *grr_task_of(struct sched_grr_entity *se)
{
	return container_of(se, struct task_struct, grr);
}

inline bool rq_in_grr_group(struct rq *rq, int g)
{
	return (g == GRR_DEFAULT) ? rq->grr_default : rq->grr_performance;
}

static void enqueue_task_grr(struct rq *rq, struct task_struct *p, int flags)
{
	struct grr_rq *grq = &rq->grr;
	struct sched_grr_entity *se = &p->grr;

	if (WARN_ON_ONCE(!list_empty(&se->run_list)))
		return;

	if (flags & ENQUEUE_WAKEUP)
		se->time_slice = GRR_TIMESLICE;

	if (flags & ENQUEUE_HEAD)
		list_add(&se->run_list, &grq->queue);
	else
		list_add_tail(&se->run_list, &grq->queue);

	grq->nr_running++;
	add_nr_running(rq, 1);
}

static bool dequeue_task_grr(struct rq *rq, struct task_struct *p, int flags)
{
	struct grr_rq *grq = &rq->grr;
	struct sched_grr_entity *se = &p->grr;

	if (WARN_ON_ONCE(list_empty(&se->run_list)))
		return true;

	list_del_init(&se->run_list);
	grq->nr_running--;
	sub_nr_running(rq, 1);
	return true;
}

static struct task_struct *pick_task_grr(struct rq *rq)
{
	struct grr_rq *grq = &rq->grr;
	struct sched_grr_entity *se;

	if (unlikely(!grq->nr_running))
		return NULL;

	se = list_first_entry(&rq->grr.queue, struct sched_grr_entity, run_list);
	return grr_task_of(se);
}

static void set_next_task_grr(struct rq *rq, struct task_struct *p, bool first)
{
	p->se.exec_start = rq_clock_task(rq);
}

static void put_prev_task_grr(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	if (prev->sched_class != &grr_sched_class)
		return;
	update_curr_common(rq);
}

static void requeue_task_grr(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_grr_entity *grr_se = &p->grr;
	struct list_head *queue = &rq->grr.queue;

	if (head)
		list_move(&grr_se->run_list, queue);
	else
		list_move_tail(&grr_se->run_list, queue);
}

static void yield_task_grr(struct rq *rq)
{
	requeue_task_grr(rq, rq->curr, 0);
}

static void update_curr_grr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	s64 delta_exec;

	if (curr->sched_class != &grr_sched_class)
		return;

	delta_exec = update_curr_common(rq);
	if (unlikely(delta_exec <= 0))
		return;

	// idk cfs does this so must be important
	schedstat_set(curr->stats.exec_max,
		      max(curr->stats.exec_max, delta_exec));
}

static void task_tick_grr(struct rq *rq, struct task_struct *curr, int queued)
{
	struct list_head *queue = &rq->grr.queue;

	update_curr_grr(rq);

	if (--curr->grr.time_slice > 0)
		return;

	curr->grr.time_slice = GRR_TIMESLICE;

	if (list_is_singular(queue))
		return;

	requeue_task_grr(rq, curr, 0);
	resched_curr(rq);
}

static void wakeup_preempt_grr(struct rq *rq, struct task_struct *p, int flags) {}
static void prio_changed_grr(struct rq *rq, struct task_struct *p, int oldprio) {}

static void switched_to_grr(struct rq *rq, struct task_struct *p)
{
	if (task_on_rq_queued(p) && sched_class_above(&grr_sched_class, rq->curr->sched_class))
		resched_curr(rq);
}

#ifdef CONFIG_SMP
static int select_task_rq_grr(struct task_struct *p, int cpu, int flags)
{
	int best = -1;
	unsigned int best_nr = UINT_MAX;
	int g = p->grr_group;
	int c;

	rcu_read_lock();
	for_each_online_cpu(c) {
		struct rq *rq = cpu_rq(c);

		if (!rq_in_grr_group(rq, g))
			continue;

		if (!task_allowed_on_cpu(p, c))
			continue;

		if (rq->grr.nr_running < best_nr) {
			best = c;
			best_nr = rq->grr.nr_running;
			if (best_nr == 0)
				break;
		}
	}
	rcu_read_unlock();

	return best >= 0 ? best : cpu;
}

static struct task_struct *pick_pushable_grr_task(struct rq *rq, int cpu)
{
	struct grr_rq *grr_rq = &rq->grr;
	struct sched_grr_entity *se;

	if (!grr_rq->nr_running)
		return NULL;

	list_for_each_entry(se, &grr_rq->queue, run_list) {
		struct task_struct *p = grr_task_of(se);

		if (!task_is_pushable(rq, p, cpu))
			continue;

		//this is needed. when migrating tasks from syscalls, they may be in a the wrong RQ
		if (!rq_in_grr_group(cpu_rq(cpu), p->grr_group))
			continue;

		return p;
	}

	return NULL;
}

static void pull_grr_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu;
	int cpu;
	struct rq *src_rq;
	struct task_struct *p;

	for_each_online_cpu(cpu) {
		if (cpu == this_cpu)
			continue;

		src_rq = cpu_rq(cpu);

		if ((src_rq->grr_default && !this_rq->grr_default) ||
		    (src_rq->grr_performance && !this_rq->grr_performance))
			continue;

		// duh
		if (src_rq->grr.nr_running < 2)
			continue;

		// one lock is held already
		double_lock_balance(this_rq, src_rq);

		// recheck
		if (src_rq->grr.nr_running < 2)
			goto unlock_next;

		if (this_rq->grr.nr_running) { // no longer need to pull
			double_unlock_balance(this_rq, src_rq);
			break;
		}

		p = pick_pushable_grr_task(src_rq, this_cpu);
		if (!p)
			goto unlock_next;

		/*
		 * we know p is queued on src_rq and can run on this_cpu.
		 * move_queued_task_locked() handles everything for us <3
		 */
		move_queued_task_locked(src_rq, this_rq, p);

		double_unlock_balance(this_rq, src_rq);
		resched_curr(this_rq);
		break;

unlock_next:
		double_unlock_balance(this_rq, src_rq);
	}
}

/* just for idle balancing
 * now, doesn't load balancing automatically do this?
 */
static int balance_grr(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	if (!rq->grr.nr_running) {
		rq_unpin_lock(rq, rf);
		pull_grr_task(rq);
		rq_repin_lock(rq, rf);
	}

	return rq->grr.nr_running > 0;
}

static void grr_balance_group(int group)
{
	struct rq *busiest = NULL, *idlest = NULL;
	struct task_struct *p = NULL;
	unsigned int busiest_nr = 0;
	unsigned int idlest_nr = UINT_MAX;
	unsigned long flags;
	int cpu;


	/* find busiest and idlest rq in this group */
	for_each_online_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		unsigned int nr;

		if (!rq_in_grr_group(rq, group))
			continue;

		nr = rq->grr.nr_running;

		if (nr > busiest_nr) {
			busiest_nr = nr;
			busiest = rq;
		}
		if (nr < idlest_nr) {
			idlest_nr = nr;
			idlest = rq;
		}
	}

	/*
	 * don't move if moving one task would reverse the imbalance.
	 * we need: busiest_nr >= idlest_nr + 2
	 */
	if (!(busiest_nr >= idlest_nr + 2))
		return;
	/*
	 * double lock and disable interrupts
	 */

	local_irq_save(flags);
	double_rq_lock(busiest, idlest);

	if (!(busiest->grr.nr_running >= idlest->grr.nr_running + 2))
		goto out_unlock;

	p = pick_pushable_grr_task(busiest, idlest->cpu);
	if (!p)
		goto out_unlock;

	move_queued_task_locked(busiest, idlest, p);
	resched_curr(busiest);
out_unlock:
	double_rq_unlock(busiest, idlest);
	local_irq_restore(flags);

}

static __latent_entropy void grr_load_balance(void)
{
	grr_balance_group(GRR_DEFAULT);
	grr_balance_group(GRR_PERFORMANCE);
}

void grr_load_balance_trigger(struct rq *rq)
{
	if (time_after_eq(jiffies, rq->grr.next_balance)) {
		rq->grr.next_balance = jiffies + msecs_to_jiffies(500);
		raise_softirq(GRR_SOFTIRQ);
	}
}
#endif /* CONFIG_SMP */
void init_sched_grr_class(void)
{
#ifdef CONFIG_SMP
	open_softirq(GRR_SOFTIRQ, grr_load_balance);
#endif
}


DEFINE_SCHED_CLASS(grr) = {
	.enqueue_task	 = enqueue_task_grr,
	.dequeue_task	 = dequeue_task_grr,
	.yield_task	   = yield_task_grr,
	.wakeup_preempt   = wakeup_preempt_grr,
	.pick_task		= pick_task_grr,
	.put_prev_task	= put_prev_task_grr,
	.set_next_task	= set_next_task_grr,

#ifdef CONFIG_SMP
	.balance = balance_grr,
	.select_task_rq   = select_task_rq_grr,
	.set_cpus_allowed = set_cpus_allowed_common,
#endif
	.update_curr = update_curr_grr,
	.task_tick		= task_tick_grr,
	.switched_to	  = switched_to_grr,
	.prio_changed		= prio_changed_grr,

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled   = 1,
#endif
};
