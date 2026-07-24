// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendored from kernel-common kernel/pid.c (kernel version 6.1.124,
 * android14-6.1 branch). See lkm4ctr/vendor_ns/README.md for the vendoring
 * rules this file follows. Only the changes marked "RENAME", "DIAGFS
 * PLUMBING" or "STANDALONE COMPILE" below differ from the pristine kernel
 * source; everything else is intentionally kept close to the original.
 */

#include <linux/mm.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/rculist.h>
#include <linux/memblock.h>
#include <linux/pid_namespace.h>
#include <linux/init_task.h>
#include <linux/syscalls.h>
#include <linux/proc_ns.h>
#include <linux/refcount.h>
#include <linux/anon_inodes.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/idr.h>
#include <net/sock.h>
#include <uapi/linux/pidfd.h>

#include "../vendor_ns.h"

static int vns_pid_max = PID_MAX_DEFAULT; /* RENAME */
static DEFINE_SPINLOCK(vns_pidmap_lock); /* RENAME */

int vns_get_pid_max(void)
{
	return READ_ONCE(vns_pid_max);
}

void vns_put_pid(struct pid *pid) /* RENAME */
{
	struct pid_namespace *ns;

	if (!pid)
		return;
	ns = pid->numbers[pid->level].ns;
	if (refcount_dec_and_test(&pid->count)) {
		kmem_cache_free(ns->pid_cachep, pid);
		vns_put_pid_ns(ns); /* RENAME */
	}
}

static void vns_delayed_put_pid(struct rcu_head *rhp) /* RENAME */
{
	struct pid *pid = container_of(rhp, struct pid, rcu);
	vns_put_pid(pid); /* RENAME */
}

void vns_free_pid(struct pid *pid) /* RENAME */
{
	int i;
	unsigned long flags;

	spin_lock_irqsave(&vns_pidmap_lock, flags);
	for (i = 0; i <= pid->level; i++) {
		struct upid *upid = pid->numbers + i;
		struct pid_namespace *ns = upid->ns;
		switch (--ns->pid_allocated) {
		case 2:
		case 1:
			wake_up_process(ns->child_reaper);
			break;
		case PIDNS_ADDING:
			WARN_ON(ns->child_reaper);
			ns->pid_allocated = 0;
			break;
		}
		idr_remove(&ns->idr, upid->nr);
	}
	spin_unlock_irqrestore(&vns_pidmap_lock, flags);
	call_rcu(&pid->rcu, vns_delayed_put_pid); /* RENAME */
}

struct pid *vns_alloc_pid(struct pid_namespace *ns, pid_t *set_tid,
			size_t set_tid_size) /* RENAME */
{
	struct pid *pid;
	enum pid_type type;
	int i, nr;
	struct pid_namespace *tmp;
	struct upid *upid;
	int retval = -ENOMEM;

	if (set_tid_size > ns->level + 1)
		return ERR_PTR(-EINVAL);
	pid = kmem_cache_alloc(ns->pid_cachep, GFP_KERNEL);
	if (!pid)
		return ERR_PTR(retval);
	tmp = ns;
	pid->level = ns->level;
	for (i = ns->level; i >= 0; i--) {
		int tid = 0;
		if (set_tid_size) {
			tid = set_tid[ns->level - i];
			retval = -EINVAL;
			if (tid < 1 || tid >= vns_pid_max)
				goto out_free;
			if (tid != 1 && !tmp->child_reaper)
				goto out_free;
			set_tid_size--;
		}
		idr_preload(GFP_KERNEL);
		spin_lock_irq(&vns_pidmap_lock);
		if (tid) {
			nr = idr_alloc(&tmp->idr, NULL, tid, tid + 1, GFP_ATOMIC);
			if (nr == -ENOSPC)
				nr = -EEXIST;
		} else {
			int pid_min = 1;
			if (idr_get_cursor(&tmp->idr) > VNS_RESERVED_PIDS)
				pid_min = VNS_RESERVED_PIDS;
			nr = idr_alloc_cyclic(&tmp->idr, NULL, pid_min,
					      vns_pid_max, GFP_ATOMIC);
		}
		spin_unlock_irq(&vns_pidmap_lock);
		idr_preload_end();
		if (nr < 0) {
			retval = (nr == -ENOSPC) ? -EAGAIN : nr;
			goto out_free;
		}
		pid->numbers[i].nr = nr;
		pid->numbers[i].ns = tmp;
		tmp = tmp->parent;
	}
	retval = -ENOMEM;
	vns_get_pid_ns(ns); /* RENAME */
	refcount_set(&pid->count, 1);
	spin_lock_init(&pid->lock);
	for (type = 0; type < PIDTYPE_MAX; ++type)
		INIT_HLIST_HEAD(&pid->tasks[type]);
	init_waitqueue_head(&pid->wait_pidfd);
	INIT_HLIST_HEAD(&pid->inodes);
	upid = pid->numbers + ns->level;
	spin_lock_irq(&vns_pidmap_lock);
	if (!(ns->pid_allocated & PIDNS_ADDING))
		goto out_unlock;
	for (; upid >= pid->numbers; --upid) {
		idr_replace(&upid->ns->idr, pid, upid->nr);
		upid->ns->pid_allocated++;
	}
	spin_unlock_irq(&vns_pidmap_lock);
	return pid;
out_unlock:
	spin_unlock_irq(&vns_pidmap_lock);
	vns_put_pid_ns(ns); /* RENAME */
out_free:
	spin_lock_irq(&vns_pidmap_lock);
	while (++i <= ns->level) {
		upid = pid->numbers + i;
		idr_remove(&upid->ns->idr, upid->nr);
	}
	if (ns->pid_allocated == PIDNS_ADDING)
		idr_set_cursor(&ns->idr, 0);
	spin_unlock_irq(&vns_pidmap_lock);
	kmem_cache_free(ns->pid_cachep, pid);
	return ERR_PTR(retval);
}

int vns_alloc_pidnr(struct pid_namespace *ns)
{
	struct pid *pid = vns_alloc_pid(ns, NULL, 0);
	int nr;

	if (IS_ERR(pid))
		return PTR_ERR(pid);
	nr = pid->numbers[pid->level].nr;
	return nr;
}

void vns_free_pidnr(struct pid_namespace *ns, int nr)
{
	struct pid *pid;

	if (nr <= 0)
		return;
	pid = idr_find(&ns->idr, nr);
	if (pid)
		vns_free_pid(pid); /* RENAME */
}

struct pid *vns_find_pid_ns(int nr, struct pid_namespace *ns)
{
	return idr_find(&ns->idr, nr);
}

struct pid *vns_find_vpid(int nr)
{
	struct pid_namespace *ns = current->nsproxy ? current->nsproxy->pid_ns_for_children : NULL;
	return ns ? vns_find_pid_ns(nr, ns) : NULL; /* RENAME */
}

static struct pid **vns_task_pid_ptr(struct task_struct *task, enum pid_type type)
{
	return (type == PIDTYPE_PID) ? &task->thread_pid : &task->signal->pids[type];
}

struct task_struct *vns_pid_task(struct pid *pid, enum pid_type type)
{
	struct task_struct *result = NULL;
	if (pid) {
		struct hlist_node *first;
		first = rcu_dereference_check(hlist_first_rcu(&pid->tasks[type]),
				      lockdep_tasklist_lock_is_held());
		if (first)
			result = hlist_entry(first, struct task_struct, pid_links[(type)]);
	}
	return result;
}

struct pid *vns_get_task_pid(struct task_struct *task, enum pid_type type)
{
	struct pid *pid;
	rcu_read_lock();
	pid = get_pid(rcu_dereference(*vns_task_pid_ptr(task, type)));
	rcu_read_unlock();
	return pid;
}

struct pid *vns_find_get_pid(pid_t nr)
{
	struct pid *pid;
	rcu_read_lock();
	pid = get_pid(vns_find_vpid(nr)); /* RENAME */
	rcu_read_unlock();
	return pid;
}

pid_t vns_pid_nr_ns(struct pid *pid, struct pid_namespace *ns)
{
	struct upid *upid;
	pid_t nr = 0;

	if (pid && ns->level <= pid->level) {
		upid = &pid->numbers[ns->level];
		if (upid->ns == ns)
			nr = upid->nr;
	}
	return nr;
}

pid_t vns_pid_vnr(struct pid *pid)
{
	struct pid_namespace *ns = current->nsproxy ? current->nsproxy->pid_ns_for_children : NULL;
	return ns ? vns_pid_nr_ns(pid, ns) : 0; /* RENAME */
}

/*
 * STANDALONE COMPILE: attach_pid(), detach_pid(), change_pid(), exchange_tids(),
 * transfer_pid(), pidfd plumbing and the syscall entry points all operate on the
 * real kernel's task_struct pid linkage or pidfd infrastructure. vendor_ns's
 * shadow-hook bookkeeping never rewires the builtin task pid lists, so that
 * portion of pristine pid.c is intentionally omitted while the core allocator and
 * lookup algorithms above remain recognisable from the original file.
 */
