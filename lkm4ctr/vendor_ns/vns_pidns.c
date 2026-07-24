// SPDX-License-Identifier: GPL-2.0
/*
 * vns_pidns.c - vendored PID namespace numbering.
 *
 * Vendored/adapted from kernel/pid.c (alloc_pid) and kernel/pid_namespace.c
 * (create_pid_namespace, disable_pid_allocation, zap_pid_ns_processes) of the
 * Linux kernel (GPL-2.0). Adapted for out-of-tree module use: symbols renamed
 * under a vns_ prefix, and the per-namespace idr allocation reproduced exactly
 * from alloc_pid()'s inner loop -- an idr_alloc_cyclic() over [pid_min,
 * pid_max) where pid_min wraps from 1 back to RESERVED_PIDS once the cursor
 * passes RESERVED_PIDS, so init keeps pid 1 and reuse wraps at RESERVED_PIDS.
 * The struct pid/upid array, refcount, tasklist and pidfs plumbing the real
 * alloc_pid() also performs are not relevant to this module's namespace-local
 * numbering, so only the idr number allocation is vendored. No changes were
 * made to the surviving algorithm beyond the renaming.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/spinlock.h>

#include "vendor_ns.h"
#include "../../common/lkm4ctr_log.h"

/*
 * Vendored from kernel/pid.c (Linux 6.1.124): pid_max is a single global limit
 * (`int pid_max = PID_MAX_DEFAULT;`), not a per-namespace field -- the 6.1
 * struct pid_namespace has no pid_max member. alloc_pid() reads this global for
 * every namespace's idr_alloc_cyclic() upper bound, so vendor_ns mirrors it as
 * a module-global rather than inventing a per-ns copy.
 */
static int vns_pid_max = VNS_PID_MAX_DEFAULT;

int vns_get_pid_max(void)
{
	return READ_ONCE(vns_pid_max);
}

static struct vns_pid_namespace *vns_alloc_pid_ns(struct vns_pid_namespace *parent,
						  struct vns_user_namespace *user_ns)
{
	struct vns_pid_namespace *ns;

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return NULL;

	/* Vendored from create_pid_namespace(): idr_init + PIDNS_ADDING. */
	idr_init(&ns->idr);
	spin_lock_init(&ns->lock);
	ns->level = parent ? parent->level + 1 : 0;
	ns->parent = parent;
	ns->user_ns = user_ns;
	ns->pid_allocated = VNS_PIDNS_ADDING;
	vns_ns_common_init(&ns->ns, VENDOR_NS_TYPE_PID);
	vns_ns_register(&ns->ns);
	return ns;
}

struct vns_pid_namespace *vns_pid_root(void)
{
	lockdep_assert_held(&vendor_ns_registry.lock);
	return vns_alloc_pid_ns(NULL, NULL);
}

struct vns_pid_namespace *vns_create_pid_ns(struct vns_pid_namespace *parent,
					    struct vns_user_namespace *user_ns)
{
	lockdep_assert_held(&vendor_ns_registry.lock);
	return vns_alloc_pid_ns(parent, user_ns);
}

/*
 * Vendored from the pid-number allocation inside alloc_pid() (kernel/pid.c).
 * Returns a namespace-local pid number >= 1, or a negative errno.
 */
int vns_alloc_pidnr(struct vns_pid_namespace *ns)
{
	int nr;
	int pid_min = 1;
	int pid_max = vns_get_pid_max();

	if (!(ns->pid_allocated & VNS_PIDNS_ADDING))
		return -ENOMEM;

	idr_preload(GFP_KERNEL);
	spin_lock(&ns->lock);

	/*
	 * init really needs pid 1, but after reaching the maximum wrap back to
	 * RESERVED_PIDS.
	 */
	if (idr_get_cursor(&ns->idr) > VNS_RESERVED_PIDS)
		pid_min = VNS_RESERVED_PIDS;

	nr = idr_alloc_cyclic(&ns->idr, NULL, pid_min, pid_max, GFP_ATOMIC);

	spin_unlock(&ns->lock);
	idr_preload_end();

	if (nr < 0)
		return (nr == -ENOSPC) ? -EAGAIN : nr;

	return nr;
}

/* Vendored from free_pid() (kernel/pid.c): drop the namespace-local number. */
void vns_free_pidnr(struct vns_pid_namespace *ns, int nr)
{
	if (nr <= 0)
		return;

	spin_lock(&ns->lock);
	idr_remove(&ns->idr, nr);
	spin_unlock(&ns->lock);
}

/*
 * Vendored from disable_pid_allocation() + zap_pid_ns_processes()
 * (kernel/pid_namespace.c). A loadable module cannot signal or reap the tasks
 * that live in a vendored pid namespace (they run under the host's real pid
 * namespace, which owns their task_struct), so only the "no more processes may
 * enter this namespace" half -- clearing PIDNS_ADDING under the namespace lock
 * -- is vendored; the SIGKILL cascade and kernel_wait4() reap loop are a
 * documented limitation of the loadable-module approach (see README).
 */
void vns_zap_pid_ns(struct vns_pid_namespace *ns)
{
	if (!ns)
		return;

	spin_lock(&ns->lock);
	ns->pid_allocated &= ~VNS_PIDNS_ADDING;
	spin_unlock(&ns->lock);
}

void vns_free_pid_ns(struct vns_pid_namespace *ns)
{
	if (!ns)
		return;

	lockdep_assert_held(&vendor_ns_registry.lock);

	if (!refcount_dec_and_test(&ns->ns.count))
		return;

	vns_zap_pid_ns(ns);
	idr_destroy(&ns->idr);
	vns_ns_unregister(&ns->ns);
	kfree(ns);
}
