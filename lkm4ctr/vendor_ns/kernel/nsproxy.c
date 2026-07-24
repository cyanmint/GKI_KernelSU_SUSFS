// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendored from kernel-common kernel/nsproxy.c (kernel version 6.1.124,
 * android14-6.1 branch). CHANGES FROM UPSTREAM:
 *   - [RENAME] All non-static global symbols prefixed with vns_ to avoid
 *     collision with the built-in kernel implementation.
 *   - [BUILD-COMPAT] slab caches replaced with kzalloc/kfree (no kmem_cache_create
 *     in out-of-tree module init context).
 *   - [BUILD-COMPAT] ns_alloc_inum/ns_free_inum -> vns_alloc_inum/vns_free_inum
 *     (proc_alloc_inum not exported; resolved at init via shadow_hook_resolve).
 *   - [BUILD-COMPAT] __init/__exit removed from non-module-init functions.
 *   - [DIAGFS] Statistics incremented via vendor_ns_registry for diagfs exposure.
 *   Any line NOT marked RENAME/BUILD-COMPAT/DIAGFS is unchanged from upstream.
 */
/*
 *  Copyright (C) 2006 IBM Corporation
 *
 *  Author: Serge Hallyn <serue@us.ibm.com>
 *
 *  Jun 2006 - namespaces support
 *             OpenVZ, SWsoft Inc.
 *             Pavel Emelianov <xemul@openvz.org>
 */

#include <linux/slab.h>
#include <linux/export.h>
#include <linux/nsproxy.h>
#include <linux/init_task.h>
#include <linux/mnt_namespace.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <net/net_namespace.h>
#include <linux/ipc_namespace.h>
#include <linux/time_namespace.h>
#include <linux/fs_struct.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/file.h>
#include <linux/syscalls.h>
#include <linux/cgroup.h>
#include <linux/perf_event.h>
#include "../vendor_ns.h"

/* [BUILD-COMPAT] Forward declaration for vendored time_namespace init object. */
extern struct time_namespace vns_init_time_ns;

/* [BUILD-COMPAT] out-of-tree vendor_ns uses kzalloc/kfree instead of a slab cache. */

struct nsproxy vns_init_nsproxy = { /* [RENAME] */
	.count			= ATOMIC_INIT(1),
	.uts_ns			= &init_uts_ns,
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
	/* [BUILD-COMPAT] init_ipc_ns is not exported; set at runtime in vendor_ns_init(). */
	.ipc_ns			= NULL,
#endif
	.mnt_ns			= NULL,
	.pid_ns_for_children	= &init_pid_ns,
#ifdef CONFIG_NET
	.net_ns			= &init_net,
#endif
#ifdef CONFIG_CGROUPS
	/* [BUILD-COMPAT] init_cgroup_ns is not exported; set at runtime via
	 * shadow_hook_resolve("init_cgroup_ns") in vendor_ns_init(). */
	.cgroup_ns		= NULL,
#endif
#ifdef CONFIG_TIME_NS
	.time_ns		= &vns_init_time_ns, /* [RENAME] init_time_ns → vns_init_time_ns */
	.time_ns_for_children	= &vns_init_time_ns, /* [RENAME] */
#endif
};

static inline struct nsproxy *create_nsproxy(void)
{
	struct nsproxy *nsproxy;

	/* [BUILD-COMPAT] no slab cache in the out-of-tree module path. */
	nsproxy = kzalloc(sizeof(*nsproxy), GFP_KERNEL);
	if (nsproxy)
		vns_init_count(&nsproxy->count, 1); /* [BUILD-COMPAT] */
	return nsproxy;
}

/*
 * Create new nsproxy and all of its the associated namespaces.
 * Return the newly created nsproxy.  Do not attach this to the task,
 * leave it to the caller to do proper locking and attach it to task.
 */
static struct nsproxy *create_new_namespaces(unsigned long flags,
	struct task_struct *tsk, struct user_namespace *user_ns,
	struct fs_struct *new_fs)
{
	struct nsproxy *new_nsp;
	int err;

	new_nsp = create_nsproxy();
	if (!new_nsp)
		return ERR_PTR(-ENOMEM);

	/* [BUILD-COMPAT] copy_mnt_ns is resolved lazily for vendor_ns. */
	if (vns_copy_mnt_ns_fn) {
		new_nsp->mnt_ns = vns_copy_mnt_ns_fn(flags, tsk->nsproxy->mnt_ns, user_ns, new_fs);
		if (IS_ERR(new_nsp->mnt_ns)) {
			err = PTR_ERR(new_nsp->mnt_ns);
			goto out_ns;
		}
	} else {
		new_nsp->mnt_ns = NULL;
	}

	new_nsp->uts_ns = vns_copy_utsname(flags, user_ns, tsk->nsproxy->uts_ns); /* [RENAME] */
	if (IS_ERR(new_nsp->uts_ns)) {
		err = PTR_ERR(new_nsp->uts_ns);
		goto out_uts;
	}

	new_nsp->ipc_ns = vns_copy_ipcs(flags, user_ns, tsk->nsproxy->ipc_ns); /* [RENAME] */
	if (IS_ERR(new_nsp->ipc_ns)) {
		err = PTR_ERR(new_nsp->ipc_ns);
		goto out_ipc;
	}

	new_nsp->pid_ns_for_children =
		vns_copy_pid_ns(flags, user_ns, tsk->nsproxy->pid_ns_for_children); /* [RENAME] */
	if (IS_ERR(new_nsp->pid_ns_for_children)) {
		err = PTR_ERR(new_nsp->pid_ns_for_children);
		goto out_pid;
	}

	new_nsp->cgroup_ns = vns_copy_cgroup_ns(flags, user_ns, /* [RENAME] */
					    tsk->nsproxy->cgroup_ns);
	if (IS_ERR(new_nsp->cgroup_ns)) {
		err = PTR_ERR(new_nsp->cgroup_ns);
		goto out_cgroup;
	}

	/* [BUILD-COMPAT] copy_net_ns is resolved lazily for vendor_ns. */
	if (vns_copy_net_ns_fn) {
		new_nsp->net_ns = vns_copy_net_ns_fn(flags, user_ns, tsk->nsproxy->net_ns);
		if (IS_ERR(new_nsp->net_ns)) {
			err = PTR_ERR(new_nsp->net_ns);
			goto out_net;
		}
	} else {
		new_nsp->net_ns = NULL;
	}

	new_nsp->time_ns_for_children = vns_copy_time_ns(flags, user_ns, /* [RENAME] */
					tsk->nsproxy->time_ns_for_children);
	if (IS_ERR(new_nsp->time_ns_for_children)) {
		err = PTR_ERR(new_nsp->time_ns_for_children);
		goto out_time;
	}
	new_nsp->time_ns = get_time_ns(tsk->nsproxy->time_ns);

	return new_nsp;

out_time:
	if (new_nsp->net_ns && vns_put_net_ns_fn)
		vns_put_net_ns_fn(new_nsp->net_ns); /* [BUILD-COMPAT] */
out_net:
	vns_put_cgroup_ns(new_nsp->cgroup_ns); /* [RENAME] */
out_cgroup:
	if (new_nsp->pid_ns_for_children)
		vns_put_pid_ns(new_nsp->pid_ns_for_children); /* [RENAME] */
out_pid:
	if (new_nsp->ipc_ns)
		vns_put_ipc_ns(new_nsp->ipc_ns); /* [RENAME] */
out_ipc:
	if (new_nsp->uts_ns)
		put_uts_ns(new_nsp->uts_ns);
out_uts:
	if (new_nsp->mnt_ns)
		if (vns_put_mnt_ns_fn)
			vns_put_mnt_ns_fn(new_nsp->mnt_ns); /* [BUILD-COMPAT] */
out_ns:
	kfree(new_nsp); /* [BUILD-COMPAT] */
	return ERR_PTR(err);
}

/*
 * called from clone.  This now handles copy for nsproxy and all
 * namespaces therein.
 */
struct nsproxy *vns_copy_namespaces(unsigned long flags, struct task_struct *tsk) /* [RENAME] */
{
	struct nsproxy *old_ns = tsk->nsproxy;
	struct user_namespace *user_ns = task_cred_xxx(tsk, user_ns);
	struct nsproxy *new_ns;

	if (likely(!(flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
			      CLONE_NEWPID | CLONE_NEWNET |
			      CLONE_NEWCGROUP | CLONE_NEWTIME)))) {
		if (likely(old_ns->time_ns_for_children == old_ns->time_ns)) {
			get_nsproxy(old_ns);
			return old_ns;
		}
	} else if (!ns_capable(user_ns, CAP_SYS_ADMIN))
		return ERR_PTR(-EPERM);

	/*
	 * CLONE_NEWIPC must detach from the undolist: after switching
	 * to a new ipc namespace, the semaphore arrays from the old
	 * namespace are unreachable.  In clone parlance, CLONE_SYSVSEM
	 * means share undolist with parent, so we must forbid using
	 * it along with CLONE_NEWIPC.
	 */
	if ((flags & (CLONE_NEWIPC | CLONE_SYSVSEM)) ==
		(CLONE_NEWIPC | CLONE_SYSVSEM))
		return ERR_PTR(-EINVAL);

	new_ns = create_new_namespaces(flags, tsk, user_ns, tsk->fs);
	if (IS_ERR(new_ns))
		return new_ns;

	vns_timens_on_fork(new_ns, tsk); /* [RENAME] */

	return new_ns;
}

void vns_free_nsproxy(struct nsproxy *ns) /* [RENAME] */
{
	if (ns->mnt_ns)
		if (vns_put_mnt_ns_fn)
			vns_put_mnt_ns_fn(ns->mnt_ns); /* [BUILD-COMPAT] */
	if (ns->uts_ns)
		put_uts_ns(ns->uts_ns);
	if (ns->ipc_ns)
		vns_put_ipc_ns(ns->ipc_ns); /* [RENAME] */
	if (ns->pid_ns_for_children)
		vns_put_pid_ns(ns->pid_ns_for_children); /* [RENAME] */
	if (ns->time_ns)
		vns_put_time_ns(ns->time_ns); /* [RENAME] */
	if (ns->time_ns_for_children)
		vns_put_time_ns(ns->time_ns_for_children); /* [RENAME] */
	vns_put_cgroup_ns(ns->cgroup_ns); /* [RENAME] */
	if (ns->net_ns && vns_put_net_ns_fn)
		vns_put_net_ns_fn(ns->net_ns); /* [BUILD-COMPAT] */
	kfree(ns); /* [BUILD-COMPAT] */
}

/*
 * Called from unshare. Unshare all the namespaces part of nsproxy.
 * On success, returns the new nsproxy.
 */
int vns_unshare_nsproxy_namespaces(unsigned long unshare_flags, /* [RENAME] */
	struct nsproxy **new_nsp, struct cred *new_cred, struct fs_struct *new_fs)
{
	struct user_namespace *user_ns;
	int err = 0;

	if (!(unshare_flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
			       CLONE_NEWNET | CLONE_NEWPID | CLONE_NEWCGROUP |
			       CLONE_NEWTIME)))
		return 0;

	user_ns = new_cred ? new_cred->user_ns : current_user_ns();
	if (!ns_capable(user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	*new_nsp = create_new_namespaces(unshare_flags, current, user_ns,
					 new_fs ? new_fs : current->fs);
	if (IS_ERR(*new_nsp)) {
		err = PTR_ERR(*new_nsp);
		goto out;
	}

out:
	return err;
}

void vns_switch_task_namespaces(struct task_struct *p, struct nsproxy *new) /* [RENAME] */
{
	struct nsproxy *ns;

	might_sleep();

	task_lock(p);
	ns = p->nsproxy;
	p->nsproxy = new;
	task_unlock(p);

	if (ns)
		vns_put_nsproxy(ns); /* [RENAME] */
}

void vns_exit_task_namespaces(struct task_struct *p) /* [RENAME] */
{
	vns_switch_task_namespaces(p, NULL);
}

static int check_setns_flags(unsigned long flags)
{
	if (!flags || (flags & ~(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
				 CLONE_NEWNET | CLONE_NEWTIME | CLONE_NEWUSER |
				 CLONE_NEWPID | CLONE_NEWCGROUP)))
		return -EINVAL;

#ifndef CONFIG_USER_NS
	if (flags & CLONE_NEWUSER)
		return -EINVAL;
#endif
#ifndef CONFIG_PID_NS
	if (flags & CLONE_NEWPID)
		return -EINVAL;
#endif
#ifndef CONFIG_UTS_NS
	if (flags & CLONE_NEWUTS)
		return -EINVAL;
#endif
#ifndef CONFIG_IPC_NS
	if (flags & CLONE_NEWIPC)
		return -EINVAL;
#endif
#ifndef CONFIG_CGROUPS
	if (flags & CLONE_NEWCGROUP)
		return -EINVAL;
#endif
#ifndef CONFIG_NET_NS
	if (flags & CLONE_NEWNET)
		return -EINVAL;
#endif
#ifndef CONFIG_TIME_NS
	if (flags & CLONE_NEWTIME)
		return -EINVAL;
#endif

	return 0;
}

static void put_nsset(struct nsset *nsset)
{
	unsigned flags = nsset->flags;

	if (flags & CLONE_NEWUSER)
		put_cred(nsset_cred(nsset));
	/*
	 * We only created a temporary copy if we attached to more than just
	 * the mount namespace.
	 */
	if (nsset->fs && (flags & CLONE_NEWNS) && (flags & ~CLONE_NEWNS))
		free_fs_struct(nsset->fs);
	if (nsset->nsproxy)
		vns_free_nsproxy(nsset->nsproxy); /* [RENAME] */
}

static int prepare_nsset(unsigned flags, struct nsset *nsset)
{
	struct task_struct *me = current;

	nsset->nsproxy = create_new_namespaces(0, me, current_user_ns(), me->fs);
	if (IS_ERR(nsset->nsproxy))
		return PTR_ERR(nsset->nsproxy);

	if (flags & CLONE_NEWUSER)
		nsset->cred = prepare_creds();
	else
		nsset->cred = current_cred();
	if (!nsset->cred)
		goto out;

	/* Only create a temporary copy of fs_struct if we really need to. */
	if (flags == CLONE_NEWNS) {
		nsset->fs = me->fs;
	} else if (flags & CLONE_NEWNS) {
		nsset->fs = copy_fs_struct(me->fs);
		if (!nsset->fs)
			goto out;
	}

	nsset->flags = flags;
	return 0;

out:
	put_nsset(nsset);
	return -ENOMEM;
}

static inline int validate_ns(struct nsset *nsset, struct ns_common *ns)
{
	return ns->ops->install(nsset, ns);
}

/*
 * This is the inverse operation to unshare().
 * Ordering is equivalent to the standard ordering used everywhere else
 * during unshare and process creation. The switch to the new set of
 * namespaces occurs at the point of no return after installation of
 * all requested namespaces was successful in commit_nsset().
 */
static int validate_nsset(struct nsset *nsset, struct pid *pid)
{
	int ret = 0;
	unsigned flags = nsset->flags;
	struct user_namespace *user_ns = NULL;
	struct pid_namespace *pid_ns = NULL;
	struct nsproxy *nsp;
	struct task_struct *tsk;

	/* Take a "snapshot" of the target task's namespaces. */
	rcu_read_lock();
	tsk = pid_task(pid, PIDTYPE_PID);
	if (!tsk) {
		rcu_read_unlock();
		return -ESRCH;
	}

	if (!ptrace_may_access(tsk, PTRACE_MODE_READ_REALCREDS)) {
		rcu_read_unlock();
		return -EPERM;
	}

	task_lock(tsk);
	nsp = tsk->nsproxy;
	if (nsp)
		get_nsproxy(nsp);
	task_unlock(tsk);
	if (!nsp) {
		rcu_read_unlock();
		return -ESRCH;
	}

#ifdef CONFIG_PID_NS
	if (flags & CLONE_NEWPID) {
		pid_ns = task_active_pid_ns(tsk);
		if (unlikely(!pid_ns)) {
			rcu_read_unlock();
			ret = -ESRCH;
			goto out;
		}
		get_pid_ns(pid_ns);
	}
#endif

#ifdef CONFIG_USER_NS
	if (flags & CLONE_NEWUSER)
		user_ns = get_user_ns(__task_cred(tsk)->user_ns);
#endif
	rcu_read_unlock();

	/*
	 * Install requested namespaces. The caller will have
	 * verified earlier that the requested namespaces are
	 * supported on this kernel. We don't report errors here
	 * if a namespace is requested that isn't supported.
	 */
#ifdef CONFIG_USER_NS
	if (flags & CLONE_NEWUSER) {
		ret = validate_ns(nsset, &user_ns->ns);
		if (ret)
			goto out;
	}
#endif

	if (flags & CLONE_NEWNS) {
		ret = validate_ns(nsset, from_mnt_ns(nsp->mnt_ns));
		if (ret)
			goto out;
	}

#ifdef CONFIG_UTS_NS
	if (flags & CLONE_NEWUTS) {
		ret = validate_ns(nsset, &nsp->uts_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_IPC_NS
	if (flags & CLONE_NEWIPC) {
		ret = validate_ns(nsset, &nsp->ipc_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_PID_NS
	if (flags & CLONE_NEWPID) {
		ret = validate_ns(nsset, &pid_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_CGROUPS
	if (flags & CLONE_NEWCGROUP) {
		ret = validate_ns(nsset, &nsp->cgroup_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_NET_NS
	if (flags & CLONE_NEWNET) {
		ret = validate_ns(nsset, &nsp->net_ns->ns);
		if (ret)
			goto out;
	}
#endif

#ifdef CONFIG_TIME_NS
	if (flags & CLONE_NEWTIME) {
		ret = validate_ns(nsset, &nsp->time_ns->ns);
		if (ret)
			goto out;
	}
#endif

out:
	if (pid_ns)
		put_pid_ns(pid_ns);
	if (nsp)
		vns_put_nsproxy(nsp); /* [RENAME] */
	put_user_ns(user_ns);

	return ret;
}

/*
 * This is the point of no return. There are just a few namespaces
 * that do some actual work here and it's sufficiently minimal that
 * a separate ns_common operation seems unnecessary for now.
 * Unshare is doing the same thing. If we'll end up needing to do
 * more in a given namespace or a helper here is ultimately not
 * exported anymore a simple commit handler for each namespace
 * should be added to ns_common.
 */
static void commit_nsset(struct nsset *nsset)
{
	unsigned flags = nsset->flags;
	struct task_struct *me = current;

#ifdef CONFIG_USER_NS
	if (flags & CLONE_NEWUSER) {
		/* transfer ownership */
		commit_creds(nsset_cred(nsset));
		nsset->cred = NULL;
	}
#endif

	/* We only need to commit if we have used a temporary fs_struct. */
	if ((flags & CLONE_NEWNS) && (flags & ~CLONE_NEWNS)) {
		set_fs_root(me->fs, &nsset->fs->root);
		set_fs_pwd(me->fs, &nsset->fs->pwd);
	}

#ifdef CONFIG_IPC_NS
	if (flags & CLONE_NEWIPC)
		exit_sem(me);
#endif

#ifdef CONFIG_TIME_NS
	if (flags & CLONE_NEWTIME)
		vns_timens_commit(me, nsset->nsproxy->time_ns); /* [RENAME] */
#endif

	/* transfer ownership */
	vns_switch_task_namespaces(me, nsset->nsproxy); /* [RENAME] */
	nsset->nsproxy = NULL;
}

long vns_sys_setns(int fd, int flags) /* [RENAME] */
{
	struct file *file;
	struct ns_common *ns = NULL;
	struct nsset nsset = {};
	int err = 0;

	file = fget(fd);
	if (!file)
		return -EBADF;

	if (proc_ns_file(file)) {
		ns = get_proc_ns(file_inode(file));
		if (flags && (ns->ops->type != flags))
			err = -EINVAL;
		flags = ns->ops->type;
	} else if (!IS_ERR(pidfd_pid(file))) {
		err = check_setns_flags(flags);
	} else {
		err = -EINVAL;
	}
	if (err)
		goto out;

	err = prepare_nsset(flags, &nsset);
	if (err)
		goto out;

	if (proc_ns_file(file))
		err = validate_ns(&nsset, ns);
	else
		err = validate_nsset(&nsset, file->private_data);
	if (!err) {
		commit_nsset(&nsset);
		vns_registry_set_nsproxy(task_tgid_nr(current), current->nsproxy); /* [DIAGFS] */
		perf_event_namespaces(current);
	}
	put_nsset(&nsset);
out:
	fput(file);
	return err;
}



void vns_put_nsproxy(struct nsproxy *ns) /* [RENAME] */
{
	if (ns && vns_put_count(&ns->count))
		vns_free_nsproxy(ns);
}
