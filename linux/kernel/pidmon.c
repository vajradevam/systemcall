// SPDX-License-Identifier: GPL-2.0
/*
 * pidmon: monitor and enforce resource limits on a process in the caller's
 * descendant tree.
 *
 *   pidmon_start(pid, flags, ulim, usize)
 *	Opens a monitoring window on the target: remembers the current values of
 *	all counters, resets the target's peak RSS / peak virtual size and arms
 *	the limits (those given in @ulim, the target's own RLIMIT_*s for the
 *	rest).  From then on the limits are enforced until the window ends.
 *
 *   pidmon_read(pid, flags, uattr, usize)
 *	Reports the window's aggregates and which limits were breached.  It
 *	never signals anything.
 *
 * Authority is ancestry: the target must be the caller's thread group or a
 * descendant of it, judged by the *current* real_parent chain at every call.
 * A target that was reparented away (orphaned) is no longer ours.
 *
 * Counters are differences against the baseline taken at start; gauges are
 * read when asked; peaks come from the kernel's own hiwater marks.  Enforcement
 * needs someone to look while the caller is not in the kernel, so a delayed work
 * item samples every live record each PIDMON_POLL_MS and runs only while there
 * is one.  When the target's last thread exits, the exit hook freezes the final
 * numbers into the record so that a short-lived child can still be read after
 * it has been reaped.
 */
#include <linux/syscalls.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/cputime.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/fdtable.h>
#include <linux/hashtable.h>
#include <linux/pid_namespace.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/resource.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/pidmon.h>
#include <uapi/linux/pidmon.h>

#define PIDMON_MAX_RECS		64	/* records per owner */
#define PIDMON_RLIM_INF		U64_MAX
#define PIDMON_POLL_MS		10	/* how often live windows are checked */
#define PIDMON_LIM_ALL		((1U << PIDMON_NLIM) - 1)

static const unsigned char pidmon_rlimit[PIDMON_NLIM] = {
	[PIDMON_LIM_CPU]	= RLIMIT_CPU,
	[PIDMON_LIM_AS]		= RLIMIT_AS,
	[PIDMON_LIM_RSS]	= RLIMIT_RSS,
	[PIDMON_LIM_DATA]	= RLIMIT_DATA,
	[PIDMON_LIM_STACK]	= RLIMIT_STACK,
	[PIDMON_LIM_MEMLOCK]	= RLIMIT_MEMLOCK,
	[PIDMON_LIM_NOFILE]	= RLIMIT_NOFILE,
};

/* absolute values of every counter we report as a window delta */
struct pidmon_cnt {
	u64 utime, stime;
	u64 vol_ctxsw, invol_ctxsw;
	u64 minor_faults, major_faults;
	u64 rchar, wchar, read_bytes, write_bytes;
};

/*
 * One record per monitored target.  It is linked on the owner's
 * signal->pidmon_recs and in pidmon_hash (keyed by target), both under
 * pidmon_lock, and is reference counted so a syscall can keep using it
 * after dropping the lock.  It names the target by struct pid, never by
 * task pointer, so it cannot dangle and a recycled pid number cannot be
 * mistaken for it.
 */
struct pidmon_rec {
	struct hlist_node	owner_node;
	struct hlist_node	hash_node;
	refcount_t		ref;
	bool			linked;
	bool			exited;		/* @final is valid */
	bool			orphaned;	/* left the owner's tree before it died */
	bool			noenforce;	/* judge and report only */
	int			sig_level;	/* 0 none, 1 warned, 2 killed */
	int			sent_sig;	/* strongest signal sent so far */
	u32			ever_soft;	/* limits breached at any time in the window */
	u32			ever_hard;
	u32			set;		/* limits given by the caller, see @lim */
	struct pidmon_rlim	lim[PIDMON_NLIM];
	struct list_head	scan_node;	/* on pidmon_poll's private list */
	struct pid		*tpid;		/* target thread group */
	struct pid		*owner;		/* owner thread group */
	u64			base_wall_ns;
	struct pidmon_cnt	base;
	struct pidmon_usage	final;
};

static DEFINE_SPINLOCK(pidmon_lock);
static DEFINE_HASHTABLE(pidmon_hash, 6);
int pidmon_nrecs;

static void pidmon_poll(struct work_struct *work);
static DECLARE_DELAYED_WORK(pidmon_dwork, pidmon_poll);

static void pidmon_rec_put(struct pidmon_rec *r)
{
	if (refcount_dec_and_test(&r->ref)) {
		put_pid(r->tpid);
		put_pid(r->owner);
		kfree(r);
	}
}

/* Caller holds pidmon_lock.  The caller drops the list's reference. */
static void pidmon_unlink_locked(struct pidmon_rec *r)
{
	if (!r->linked)
		return;
	r->linked = false;
	hlist_del_init(&r->owner_node);
	hash_del(&r->hash_node);
	WRITE_ONCE(pidmon_nrecs, pidmon_nrecs - 1);
}

static void pidmon_drop(struct pidmon_rec *r)
{
	bool was;

	spin_lock(&pidmon_lock);
	was = r->linked;
	pidmon_unlink_locked(r);
	spin_unlock(&pidmon_lock);
	if (was)
		pidmon_rec_put(r);
}

/*
 * Is @t in the tree rooted at thread group @anc (or is it @anc itself)?
 * Walks real_parent, the genealogy; ->parent can be changed by ptrace.
 */
static bool pidmon_in_tree(struct task_struct *t, struct pid *anc)
{
	lockdep_assert_in_rcu_read_lock();

	while (t && t != &init_task) {
		if (task_tgid(t) == anc)
			return true;
		t = rcu_dereference(t->real_parent);
	}
	return false;
}

/*
 * Resolve the pid number the caller passed (0 = itself) to a pinned thread
 * group leader and its struct pid, and check authority.
 */
static int pidmon_get_target(pid_t nr, struct task_struct **tp, struct pid **pp)
{
	struct task_struct *t = NULL;
	struct pid *p;
	int ret = 0;

	rcu_read_lock();
	p = nr ? find_vpid(nr) : task_tgid(current);
	if (p)
		t = pid_task(p, PIDTYPE_TGID);	/* NULL for a non-leader tid */
	if (!t) {
		ret = -ESRCH;
	} else if (!pidmon_in_tree(t, task_tgid(current))) {
		ret = -EPERM;
	} else {
		get_task_struct(t);
		*tp = t;
		*pp = get_pid(p);
	}
	rcu_read_unlock();
	return ret;
}

/*
 * Find the caller's record for @nr.  A live (or unreaped) target is matched
 * by struct pid; a target that is gone and whose number is unused is matched
 * by the number it had.  Caller holds rcu and pidmon_lock.
 */
static struct pidmon_rec *pidmon_find_locked(pid_t nr)
{
	struct pid *p = nr ? find_vpid(nr) : task_tgid(current);
	struct pid_namespace *ns = task_active_pid_ns(current);
	struct pidmon_rec *r;

	hlist_for_each_entry(r, &current->signal->pidmon_recs, owner_node) {
		if (p) {
			if (r->tpid == p)
				return r;
		} else if (r->exited && pid_nr_ns(r->tpid, ns) == nr) {
			return r;
		}
	}
	return NULL;
}

/* any thread of the group that still has an mm; the leader may be gone */
static struct mm_struct *pidmon_get_mm(struct task_struct *t)
{
	struct task_struct *th;
	struct mm_struct *mm = NULL;

	rcu_read_lock();
	for_each_thread(t, th) {
		mm = get_task_mm(th);
		if (mm)
			break;
	}
	rcu_read_unlock();
	return mm;
}

static u64 pidmon_count_fds(struct task_struct *t)
{
	struct task_struct *th;
	struct fdtable *fdt;
	u64 n = 0;

	rcu_read_lock();
	for_each_thread(t, th) {
		task_lock(th);
		if (th->files) {
			fdt = files_fdtable(th->files);
			n = bitmap_weight(fdt->open_fds, fdt->max_fds);
			task_unlock(th);
			break;
		}
		task_unlock(th);
	}
	rcu_read_unlock();
	return n;
}

static void pidmon_read_counters(struct task_struct *t, struct pidmon_cnt *c)
{
	struct signal_struct *sig = t->signal;
	struct task_io_accounting acct;
	struct task_struct *th;
	u64 ut, st;

	memset(c, 0, sizeof(*c));
	thread_group_cputime_adjusted(t, &ut, &st);
	c->utime = ut;
	c->stime = st;

	/* same sampling as getrusage(RUSAGE_SELF) and /proc/<pid>/io */
	guard(rcu)();
	scoped_seqlock_read(&sig->stats_lock, ss_lock_irqsave) {
		c->vol_ctxsw = sig->nvcsw;
		c->invol_ctxsw = sig->nivcsw;
		c->minor_faults = sig->min_flt;
		c->major_faults = sig->maj_flt;
		acct = sig->ioac;
		__for_each_thread(sig, th) {
			c->vol_ctxsw += th->nvcsw;
			c->invol_ctxsw += th->nivcsw;
			c->minor_faults += th->min_flt;
			c->major_faults += th->maj_flt;
			task_io_accounting_add(&acct, &th->ioac);
		}
	}
#ifdef CONFIG_TASK_XACCT
	c->rchar = acct.rchar;
	c->wchar = acct.wchar;
#endif
#ifdef CONFIG_TASK_IO_ACCOUNTING
	c->read_bytes = acct.read_bytes;
	c->write_bytes = acct.write_bytes;
#endif
}

static u64 pidmon_delta(u64 now, u64 base)
{
	return now > base ? now - base : 0;
}

static u64 pidmon_rlim_val(unsigned long v)
{
	return v == RLIM_INFINITY ? PIDMON_RLIM_INF : v;
}

/*
 * Fill @u for target @t (a group member) at time @now, relative to the
 * baseline and with the limits in @r.  @mm may be NULL (zombie target): memory
 * fields stay 0.  Does not sleep.
 */
static void pidmon_fill(struct task_struct *t, struct mm_struct *mm,
			const struct pidmon_rec *r, u64 now,
			struct pidmon_usage *u)
{
	struct pidmon_cnt c;
	u64 cpu_delta;
	int i;

	BUILD_BUG_ON(sizeof(*u) != PIDMON_USAGE_SIZE_VER0);

	memset(u, 0, sizeof(*u));
	u->size = sizeof(*u);
	u->wall_ns = pidmon_delta(now, r->base_wall_ns);

	pidmon_read_counters(t, &c);
	u->utime_ns = pidmon_delta(c.utime, r->base.utime);
	u->stime_ns = pidmon_delta(c.stime, r->base.stime);
	u->cpu_total_ns = c.utime + c.stime;
	cpu_delta = u->utime_ns + u->stime_ns;
	u->cpu_permille = u->wall_ns ? div64_u64(cpu_delta * 1000, u->wall_ns) : 0;
	u->vol_ctxsw = pidmon_delta(c.vol_ctxsw, r->base.vol_ctxsw);
	u->invol_ctxsw = pidmon_delta(c.invol_ctxsw, r->base.invol_ctxsw);
	u->minor_faults = pidmon_delta(c.minor_faults, r->base.minor_faults);
	u->major_faults = pidmon_delta(c.major_faults, r->base.major_faults);
	u->rchar = pidmon_delta(c.rchar, r->base.rchar);
	u->wchar = pidmon_delta(c.wchar, r->base.wchar);
	u->read_bytes = pidmon_delta(c.read_bytes, r->base.read_bytes);
	u->write_bytes = pidmon_delta(c.write_bytes, r->base.write_bytes);

	if (mm) {
		unsigned long anon = get_mm_counter_sum(mm, MM_ANONPAGES);
		unsigned long file = get_mm_counter_sum(mm, MM_FILEPAGES);
		unsigned long shmem = get_mm_counter_sum(mm, MM_SHMEMPAGES);
		unsigned long rss = anon + file + shmem;
		/* the hiwater marks are updated locklessly by their writers, as /proc reads them */
		unsigned long hw_rss = data_race(mm->hiwater_rss);
		/* same lockless read */
		unsigned long hw_vm = data_race(mm->hiwater_vm);

		u->rss_bytes = (u64)rss << PAGE_SHIFT;
		u->rss_anon = (u64)anon << PAGE_SHIFT;
		u->rss_file = (u64)file << PAGE_SHIFT;
		u->rss_peak = (u64)max(hw_rss, rss) << PAGE_SHIFT;
		u->vm_bytes = (u64)mm->total_vm << PAGE_SHIFT;
		u->vm_peak = (u64)max(hw_vm, mm->total_vm) << PAGE_SHIFT;
		u->data_vm = (u64)mm->data_vm << PAGE_SHIFT;
		u->stack_vm = (u64)mm->stack_vm << PAGE_SHIFT;
		u->locked_vm = (u64)mm->locked_vm << PAGE_SHIFT;
	}

	u->nr_threads = get_nr_threads(t);
	u->nr_open_fds = pidmon_count_fds(t);

	for (i = 0; i < PIDMON_NLIM; i++) {
		if (r->set & BIT(i)) {
			u->lim[i] = r->lim[i];
		} else {
			u->lim[i].soft = pidmon_rlim_val(task_rlimit(t, pidmon_rlimit[i]));
			u->lim[i].hard = pidmon_rlim_val(task_rlimit_max(t, pidmon_rlimit[i]));
		}
	}
}

/*
 * Compare the usage in @u with its limits.  Sets the exceeded masks (as of
 * now) and returns the signal that enforcement calls for (0 for none), with
 * its severity in *level: 1 = warn (soft), 2 = kill (hard).
 */
static int pidmon_judge(struct pidmon_usage *u, int *level)
{
	u64 use[PIDMON_NLIM] = {
		[PIDMON_LIM_CPU]	= div_u64(u->cpu_total_ns, NSEC_PER_SEC),
		[PIDMON_LIM_AS]		= u->vm_bytes,
		[PIDMON_LIM_RSS]	= u->rss_bytes,
		[PIDMON_LIM_DATA]	= u->data_vm,
		[PIDMON_LIM_STACK]	= u->stack_vm,
		[PIDMON_LIM_MEMLOCK]	= u->locked_vm,
		[PIDMON_LIM_NOFILE]	= u->nr_open_fds,
	};
	int i;

	*level = 0;
	for (i = 0; i < PIDMON_NLIM; i++) {
		u64 soft = u->lim[i].soft, hard = u->lim[i].hard;
		/* the kernel's own RLIMIT_CPU test is ">=", the others are ">" */
		bool cpu = i == PIDMON_LIM_CPU;

		if (soft != PIDMON_RLIM_INF && (cpu ? use[i] >= soft : use[i] > soft))
			u->exceeded_soft |= BIT(i);
		if (hard != PIDMON_RLIM_INF && (cpu ? use[i] >= hard : use[i] > hard))
			u->exceeded_hard |= BIT(i);
	}

	if (u->exceeded_hard) {
		*level = 2;
		return SIGKILL;
	}
	if (u->exceeded_soft) {
		*level = 1;
		return (u->exceeded_soft & BIT(PIDMON_LIM_CPU)) ? SIGXCPU : SIGTERM;
	}
	return 0;
}

/* Has the last thread of @t's group begun exiting?  It may still be a zombie. */
static bool pidmon_group_dead(struct task_struct *t)
{
	return !atomic_read(&t->signal->live);
}

/* Is there already a live record for @tpid, or too many for this owner? */
static int pidmon_check_room_locked(struct pid *tpid)
{
	struct pidmon_rec *r;
	int n = 0;

	hash_for_each_possible(pidmon_hash, r, hash_node, (unsigned long)tpid)
		if (r->tpid == tpid && !r->exited)
			return -EBUSY;

	hlist_for_each_entry(r, &current->signal->pidmon_recs, owner_node)
		n++;
	return n >= PIDMON_MAX_RECS ? -ENOSPC : 0;
}

/* Fold what happened over the whole window into a report. */
static void pidmon_merge_locked(const struct pidmon_rec *r, struct pidmon_usage *u)
{
	u->exceeded_soft |= r->ever_soft;
	u->exceeded_hard |= r->ever_hard;
	u->signal_sent = r->sent_sig;
}

/*
 * One look at a live record: note any breach and, unless the window is report-only,
 * answer it.  Each severity is answered once, so a target that stays over its soft
 * limit is warned once, and one that later passes its hard limit is still killed.
 */
static void pidmon_check(struct pidmon_rec *r)
{
	struct task_struct *t;
	struct pidmon_usage u;
	struct mm_struct *mm;
	int sig, level, send = 0;

	rcu_read_lock();
	t = pid_task(r->tpid, PIDTYPE_TGID);
	/* ancestry is judged all the time: a target that left our tree is not ours to signal */
	if (!t || !pidmon_in_tree(t, r->owner)) {
		rcu_read_unlock();
		return;
	}
	get_task_struct(t);
	rcu_read_unlock();

	mm = pidmon_get_mm(t);
	pidmon_fill(t, mm, r, ktime_get_ns(), &u);
	if (mm)
		mmput(mm);
	put_task_struct(t);

	sig = pidmon_judge(&u, &level);

	spin_lock(&pidmon_lock);
	if (r->linked && !r->exited) {	/* stopped or frozen meanwhile: leave it be */
		r->ever_soft |= u.exceeded_soft;
		r->ever_hard |= u.exceeded_hard;
		if (sig && !r->noenforce && r->sig_level < level) {
			r->sig_level = level;
			r->sent_sig = send = sig;
		}
	}
	spin_unlock(&pidmon_lock);

	/* ancestry is the authorization, so no uid / CAP_KILL check */
	if (send)
		kill_pid(r->tpid, send, 1);
}

static void pidmon_poll(struct work_struct *work)
{
	struct pidmon_rec *r, *next;
	LIST_HEAD(scan);
	int bkt;

	spin_lock(&pidmon_lock);
	hash_for_each(pidmon_hash, bkt, r, hash_node) {	/* live records only: exited ones leave it */
		refcount_inc(&r->ref);
		list_add_tail(&r->scan_node, &scan);
	}
	spin_unlock(&pidmon_lock);

	if (list_empty(&scan))
		return;		/* pidmon_start() calls us again when there is something to watch */

	list_for_each_entry_safe(r, next, &scan, scan_node) {
		list_del(&r->scan_node);
		pidmon_check(r);
		pidmon_rec_put(r);
	}
	schedule_delayed_work(&pidmon_dwork, msecs_to_jiffies(PIDMON_POLL_MS));
}

SYSCALL_DEFINE4(pidmon_start, pid_t, pid, unsigned int, flags,
		const struct pidmon_limits __user *, ulim, size_t, usize)
{
	struct pidmon_limits lim = {};
	struct task_struct *t;
	struct pidmon_rec *r;
	struct mm_struct *mm;
	struct pid *tpid;
	int i, ret;

	if ((flags & ~PIDMON_START_VALID) || pid < 0)
		return -EINVAL;
	if (ulim) {
		if (usize > PAGE_SIZE)
			return -E2BIG;
		if (usize < PIDMON_LIMITS_SIZE_VER0)
			return -EINVAL;
		ret = copy_struct_from_user(&lim, sizeof(lim), ulim, usize);
		if (ret)
			return ret;
		if (lim.set & ~PIDMON_LIM_ALL)
			return -EINVAL;
		for (i = 0; i < PIDMON_NLIM; i++)
			if ((lim.set & BIT(i)) && lim.lim[i].soft > lim.lim[i].hard)
				return -EINVAL;
	} else if (usize) {
		return -EINVAL;
	}

	r = kzalloc_obj(*r);
	if (!r)
		return -ENOMEM;
	refcount_set(&r->ref, 1);
	r->noenforce = flags & PIDMON_F_NO_ENFORCE;
	r->set = lim.set;
	memcpy(r->lim, lim.lim, sizeof(r->lim));

	ret = pidmon_get_target(pid, &t, &tpid);
	if (ret)
		goto out_free;

	/* refuse before touching the target's peak marks */
	spin_lock(&pidmon_lock);
	ret = pidmon_check_room_locked(tpid);
	if (!ret && pidmon_group_dead(t))
		ret = -ESRCH;
	spin_unlock(&pidmon_lock);
	if (ret)
		goto out_put;

	mm = pidmon_get_mm(t);
	if (mm) {
		/* restart the peaks, as writing 5 to /proc/<pid>/clear_refs does */
		if (mmap_write_lock_killable(mm)) {
			mmput(mm);
			ret = -EINTR;
			goto out_put;
		}
		reset_mm_hiwater_rss(mm);
		mm->hiwater_vm = mm->total_vm;
		mmap_write_unlock(mm);
		mmput(mm);
	}

	r->base_wall_ns = ktime_get_ns();
	pidmon_read_counters(t, &r->base);
	r->tpid = tpid;
	r->owner = get_pid(task_tgid(current));
	r->linked = true;

	spin_lock(&pidmon_lock);
	ret = pidmon_check_room_locked(tpid);	/* lost a race with another start? */
	/*
	 * The exit hook runs after the group's live count reaches zero and looks for a record under
	 * this lock.  So if the group is still live here, the hook will find the record we link; if it
	 * is dead, the hook may already have passed and the record would never be frozen or released.
	 */
	if (!ret && pidmon_group_dead(t))
		ret = -ESRCH;
	if (!ret) {
		hlist_add_head(&r->owner_node, &current->signal->pidmon_recs);
		hash_add(pidmon_hash, &r->hash_node, (unsigned long)tpid);
		WRITE_ONCE(pidmon_nrecs, pidmon_nrecs + 1);
	}
	spin_unlock(&pidmon_lock);
	if (ret) {
		r->linked = false;
		goto out_put;
	}
	put_task_struct(t);
	/* a no-op if the poll is already queued; it queues itself again while it finds work */
	schedule_delayed_work(&pidmon_dwork, msecs_to_jiffies(PIDMON_POLL_MS));
	return 0;

out_put:
	put_task_struct(t);
	if (!r->tpid)
		put_pid(tpid);	/* not yet owned by the record */
out_free:
	pidmon_rec_put(r);
	return ret;
}

SYSCALL_DEFINE4(pidmon_read, pid_t, pid, unsigned int, flags,
		struct pidmon_usage __user *, uattr, size_t, usize)
{
	struct task_struct *t = NULL;
	struct pidmon_usage u;
	struct pidmon_rec *r;
	struct mm_struct *mm;
	int level, ret = 0;
	bool exited = false, orphaned = false;

	if ((flags & ~PIDMON_READ_VALID) || pid < 0)
		return -EINVAL;
	if (usize > PAGE_SIZE)
		return -E2BIG;
	if (usize < PIDMON_USAGE_SIZE_VER0)
		return -EINVAL;

	rcu_read_lock();
	spin_lock(&pidmon_lock);
	r = pidmon_find_locked(pid);
	if (r) {
		refcount_inc(&r->ref);
		exited = r->exited;
		if (exited) {
			u = r->final;
			orphaned = r->orphaned;
		}
	}
	spin_unlock(&pidmon_lock);
	rcu_read_unlock();
	if (!r)
		return -ENOENT;

	if (exited) {
		if (orphaned) {		/* it left our tree before it died */
			pidmon_drop(r);
			ret = -EPERM;
			goto out;
		}
	} else {
		rcu_read_lock();
		t = pid_task(r->tpid, PIDTYPE_TGID);
		if (!t) {
			ret = -ESRCH;	/* gone, and no final numbers were kept */
		} else if (!pidmon_in_tree(t, task_tgid(current))) {
			t = NULL;
			ret = -EPERM;
		} else {
			get_task_struct(t);
		}
		rcu_read_unlock();
		if (!t) {
			if (ret == -ESRCH)
				pidmon_drop(r);
			goto out;
		}

		mm = pidmon_get_mm(t);
		pidmon_fill(t, mm, r, ktime_get_ns(), &u);
		if (mm)
			mmput(mm);
		if (!atomic_read(&t->signal->live))
			u.state |= PIDMON_ST_EXITED;

		pidmon_judge(&u, &level);	/* the masks as of now; reading never signals */
		spin_lock(&pidmon_lock);
		pidmon_merge_locked(r, &u);
		spin_unlock(&pidmon_lock);
	}

	u.size = min_t(size_t, usize, sizeof(u));
	ret = copy_struct_to_user(uattr, usize, &u, sizeof(u), NULL);
	if (ret)
		goto out;	/* no side effects on a failed call */

	if (flags & PIDMON_F_STOP)
		pidmon_drop(r);
out:
	if (t)
		put_task_struct(t);
	pidmon_rec_put(r);
	return ret;
}

/*
 * The last thread of a thread group is exiting (called before exit_mm()).
 * As a target: freeze the final numbers into the record.  As an owner: free
 * every record this group still holds.
 */
void pidmon_exit_group(struct task_struct *tsk)
{
	struct pid *tpid = task_tgid(tsk);
	struct pidmon_rec *r = NULL, *c;
	struct hlist_head dead = HLIST_HEAD_INIT;
	struct hlist_node *tmp;
	struct pidmon_usage u;
	bool orphaned;
	int level;

	spin_lock(&pidmon_lock);
	hash_for_each_possible(pidmon_hash, c, hash_node, (unsigned long)tpid) {
		if (c->tpid == tpid && !c->exited) {
			r = c;
			refcount_inc(&r->ref);
			break;
		}
	}
	spin_unlock(&pidmon_lock);

	if (r) {
		pidmon_fill(tsk, tsk->mm, r, ktime_get_ns(), &u);
		u.state |= PIDMON_ST_EXITED;
		pidmon_judge(&u, &level);	/* masks only, a dead target is not signalled */

		rcu_read_lock();
		orphaned = !pidmon_in_tree(tsk->group_leader, r->owner);
		rcu_read_unlock();

		spin_lock(&pidmon_lock);
		if (r->linked) {
			pidmon_merge_locked(r, &u);
			r->final = u;
			r->orphaned = orphaned;
			r->exited = true;
			hash_del(&r->hash_node);
		}
		spin_unlock(&pidmon_lock);
		pidmon_rec_put(r);
	}

	spin_lock(&pidmon_lock);
	hlist_for_each_entry_safe(c, tmp, &tsk->signal->pidmon_recs, owner_node) {
		pidmon_unlink_locked(c);
		hlist_add_head(&c->owner_node, &dead);
	}
	spin_unlock(&pidmon_lock);

	hlist_for_each_entry_safe(c, tmp, &dead, owner_node) {
		hlist_del_init(&c->owner_node);
		pidmon_rec_put(c);	/* the list's reference */
	}
}
