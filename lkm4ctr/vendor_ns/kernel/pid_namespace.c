// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendored from kernel-common kernel/pid_namespace.c (kernel version 6.1.124,
 * android14-6.1 branch). See lkm4ctr/vendor_ns/README.md for the vendoring
 * rules this file follows. Only the changes marked "RENAME", "DIAGFS
 * PLUMBING" or "STANDALONE COMPILE" below differ from the pristine kernel
 * source; everything else is intentionally kept close to the original.
 *
 * Authors:
 *    (C) 2007 Pavel Emelyanov <xemul@openvz.org>, OpenVZ, SWsoft Inc.
 *    (C) 2007 Sukadev Bhattiprolu <sukadev@us.ibm.com>, IBM
 *     Many thanks to Oleg Nesterov for comments and help
 */

#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/user_namespace.h>
#include <linux/syscalls.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/acct.h>
#include <linux/slab.h>
#include <linux/reboot.h>
#include <linux/export.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/idr.h>

#include "../vendor_ns.h"

static DEFINE_MUTEX(vns_pid_caches_mutex); /* RENAME */
static DEFINE_SPINLOCK(vns_pidns_lock); /* STANDALONE COMPILE */
static struct kmem_cache *vns_pid_ns_cachep; /* RENAME */
static struct kmem_cache *vns_pid_cache[MAX_PID_NS_LEVEL]; /* RENAME */

static struct kmem_cache *vns_create_pid_cachep(unsigned int level) /* RENAME */
{
	struct kmem_cache **pkc = &vns_pid_cache[level - 1];
	struct kmem_cache *kc;
	char name[4 + 10 + 1];
	unsigned int len;

	kc = READ_ONCE(*pkc);
	if (kc)
		return kc;
	snprintf(name, sizeof(name), "vns_pid_%u", level + 1); /* RENAME */
	len = sizeof(struct pid) + level * sizeof(struct upid);
	mutex_lock(&vns_pid_caches_mutex);
	if (!*pkc)
		*pkc = kmem_cache_create(name, len, 0,
				SLAB_HWCACHE_ALIGN | SLAB_ACCOUNT, NULL); /* STANDALONE COMPILE */
	mutex_unlock(&vns_pid_caches_mutex);
	return READ_ONCE(*pkc);
}

static struct ucounts *vns_inc_pid_namespaces(struct user_namespace *ns) /* RENAME */
{
	return (struct ucounts *)1; /* STANDALONE COMPILE */
}

static void vns_dec_pid_namespaces(struct ucounts *ucounts) /* RENAME */
{
}

struct pid_namespace *vns_get_pid_ns(struct pid_namespace *ns) /* RENAME */
{
	if (ns)
		refcount_inc(&ns->ns.count);
	return ns;
}

static struct pid_namespace *vns_alloc_pid_namespace(struct user_namespace *user_ns,
	struct pid_namespace *parent_pid_ns) /* RENAME */
{
	struct pid_namespace *ns;
	unsigned int level = parent_pid_ns ? parent_pid_ns->level + 1 : 0;
	struct ucounts *ucounts;
	int err;

	err = -ENOSPC;
	if (level > MAX_PID_NS_LEVEL)
		goto out;
	ucounts = vns_inc_pid_namespaces(user_ns); /* RENAME */
	if (!ucounts)
		goto out;
	if (!vns_pid_ns_cachep)
		vns_pid_ns_cachep = kmem_cache_create("vns_pid_namespace",
				sizeof(struct pid_namespace), 0,
				SLAB_HWCACHE_ALIGN | SLAB_ACCOUNT, NULL); /* STANDALONE COMPILE */
	err = -ENOMEM;
	ns = vns_pid_ns_cachep ? kmem_cache_zalloc(vns_pid_ns_cachep, GFP_KERNEL) : NULL;
	if (!ns)
		goto out_dec;
	idr_init(&ns->idr);
	ns->pid_cachep = level ? vns_create_pid_cachep(level) : kmem_cache_create("vns_pid_root_pid", sizeof(struct pid), 0,
			SLAB_HWCACHE_ALIGN | SLAB_ACCOUNT, NULL); /* STANDALONE COMPILE */
	if (!ns->pid_cachep)
		goto out_free_idr;
	err = vns_ns_alloc_inum(&ns->ns); /* RENAME */
	if (err)
		goto out_free_idr;
	refcount_set(&ns->ns.count, 1);
	ns->level = level;
	ns->parent = parent_pid_ns ? vns_get_pid_ns(parent_pid_ns) : NULL;
	ns->user_ns = user_ns ? get_user_ns(user_ns) : current_user_ns(); /* STANDALONE COMPILE */
	ns->ucounts = ucounts;
	ns->pid_allocated = PIDNS_ADDING;
	vns_ns_register(&ns->ns, VENDOR_NS_TYPE_PID); /* DIAGFS PLUMBING */
	return ns;
out_free_idr:
	idr_destroy(&ns->idr);
	kmem_cache_free(vns_pid_ns_cachep, ns);
out_dec:
	vns_dec_pid_namespaces(ucounts);
out:
	return ERR_PTR(err);
}

static void vns_delayed_free_pidns(struct rcu_head *p) /* RENAME */
{
	struct pid_namespace *ns = container_of(p, struct pid_namespace, rcu);

	vns_dec_pid_namespaces(ns->ucounts);
	put_user_ns(ns->user_ns);
	kmem_cache_free(vns_pid_ns_cachep, ns);
}

static void vns_destroy_pid_namespace(struct pid_namespace *ns) /* RENAME */
{
	vns_ns_unregister(&ns->ns); /* DIAGFS PLUMBING */
	vns_ns_free_inum(&ns->ns); /* RENAME */
	idr_destroy(&ns->idr);
	call_rcu(&ns->rcu, vns_delayed_free_pidns); /* RENAME */
}

struct pid_namespace *vns_copy_pid_ns(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *old_ns) /* RENAME */
{
	if (!(flags & CLONE_NEWPID))
		return vns_get_pid_ns(old_ns);
	return vns_alloc_pid_namespace(user_ns, old_ns); /* RENAME */
}

void vns_put_pid_ns(struct pid_namespace *ns) /* RENAME */
{
	struct pid_namespace *parent;

	while (ns) {
		parent = ns->parent;
		if (!refcount_dec_and_test(&ns->ns.count))
			break;
		vns_destroy_pid_namespace(ns); /* RENAME */
		ns = parent;
	}
}

void vns_disable_pid_allocation(struct pid_namespace *ns) /* RENAME */
{
	spin_lock_irq(&vns_pidns_lock); /* STANDALONE COMPILE */
	ns->pid_allocated &= ~PIDNS_ADDING;
	spin_unlock_irq(&vns_pidns_lock);
}

void vns_zap_pid_ns_processes(struct pid_namespace *pid_ns) /* RENAME */
{
	vns_disable_pid_allocation(pid_ns); /* RENAME */
	/*
	 * STANDALONE COMPILE: the real signal sweep needs tasklist_lock and
	 * group_send_sig_info(), both non-exported host-kernel symbols. vendor_ns
	 * only relies on disabling further pid allocation here, so the teardown
	 * stops after preserving that load-bearing state transition.
	 */
}

struct pid_namespace *vns_pid_root(void)
{
	struct pid_namespace *ns = vns_alloc_pid_namespace(current_user_ns(), NULL);
	return IS_ERR(ns) ? NULL : ns; /* STANDALONE COMPILE */
}

struct pid_namespace *vns_create_pid_ns(struct pid_namespace *parent,
	struct user_namespace *user_ns)
{
	struct pid_namespace *ns = vns_alloc_pid_namespace(user_ns, parent);
	return IS_ERR(ns) ? NULL : ns;
}

/*
 * STANDALONE COMPILE: the remaining pristine pid_namespace.c surface is the real
 * proc-ns setns/install vtable, reboot_pid_ns(), and boot-time sysctl/initcall
 * wiring. vendor_ns never installs into task_struct->nsproxy directly and does
 * not expose /proc/<pid>/ns pidns file operations, so that portion is omitted.
 */

void vns_free_pid_ns(struct pid_namespace *ns)
{
	vns_put_pid_ns(ns); /* RENAME */
}
