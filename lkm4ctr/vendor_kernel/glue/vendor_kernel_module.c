// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_module.c - vendor_kernel lifecycle: init/exit and hook installation.
 * This is NEW code (not vendored from kernel-common).
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hashtable.h>
#include <linux/atomic.h>

#include "../vendor_kernel.h"
#include "../../shadow_ns/shadow_ns_internal.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

int (*vns_proc_alloc_inum_fn)(unsigned int *);
void (*vns_proc_free_inum_fn)(unsigned int);
struct mnt_namespace *(*vns_copy_mnt_ns_fn)(unsigned long, struct mnt_namespace *, struct user_namespace *, struct fs_struct *);
void (*vns_put_mnt_ns_fn)(struct mnt_namespace *);
struct net *(*vns_copy_net_ns_fn)(unsigned long, struct user_namespace *, struct net *);
void (*vns_put_net_ns_fn)(struct net *);
bool vendor_kernel_enabled;

struct vns_registry vendor_kernel_registry;
static atomic_t vns_inum_counter = ATOMIC_INIT(0x60000000);

int vns_alloc_inum(struct ns_common *ns)
{
	vns_zero_stashed(ns); /* [BUILD-COMPAT] */
	if (vns_proc_alloc_inum_fn)
		return vns_proc_alloc_inum_fn(&ns->inum);
	ns->inum = (unsigned int)atomic_inc_return(&vns_inum_counter);
	return 0;
}

void vns_free_inum(struct ns_common *ns)
{
	if (vns_proc_free_inum_fn && ns->inum)
		vns_proc_free_inum_fn(ns->inum);
}

static struct vns_task *__vns_task_find_locked(pid_t tgid)
{
	struct vns_task *t;
	hash_for_each_possible(vendor_kernel_registry.tasks, t, node, (unsigned long)tgid)
		if (t->tgid == tgid)
			return t;
	return NULL;
}

struct vns_task *vns_task_find(pid_t tgid)
{
	struct vns_task *t;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	t = __vns_task_find_locked(tgid);
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	return t;
}

struct nsproxy *vns_current_nsproxy(void)
{
	struct vns_task *t = vns_task_find(task_tgid_nr(current));
	return t ? t->nsproxy : NULL;
}

struct ipc_namespace *vns_task_ipc_ns(struct task_struct *task)
{
	struct vns_task *t = vns_task_find(task_tgid_nr(task));

	if (t && t->nsproxy && t->nsproxy->ipc_ns)
		return t->nsproxy->ipc_ns;
	return task->nsproxy->ipc_ns;
}

int vns_registry_set_nsproxy(pid_t tgid, struct nsproxy *nsproxy)
{
	struct vns_task *t;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	t = __vns_task_find_locked(tgid);
	if (!t) {
		t = kzalloc(sizeof(*t), GFP_ATOMIC);
		if (!t) {
			spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
			return -ENOMEM;
		}
		t->tgid = tgid;
		hash_add(vendor_kernel_registry.tasks, &t->node, (unsigned long)tgid);
		vendor_kernel_registry.task_count++;
	}
	if (nsproxy)
		get_nsproxy(nsproxy);
	if (t->nsproxy)
		vns_put_nsproxy(t->nsproxy);
	t->nsproxy = nsproxy;
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	return 0;
}

void vns_registry_remove(pid_t tgid)
{
	struct vns_task *t;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	t = __vns_task_find_locked(tgid);
	if (t) {
		hash_del(&t->node);
		if (vendor_kernel_registry.task_count)
			vendor_kernel_registry.task_count--;
	}
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	if (!t)
		return;
	if (t->nsproxy)
		vns_put_nsproxy(t->nsproxy);
	kfree(t);
}

void vns_registry_clone(pid_t parent_tgid, pid_t child_tgid)
{
	struct nsproxy *nsproxy = NULL;
	struct vns_task *t;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	t = __vns_task_find_locked(parent_tgid);
	if (t && t->nsproxy) {
		get_nsproxy(t->nsproxy);
		nsproxy = t->nsproxy;
	}
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	if (!nsproxy)
		return;
	vns_registry_set_nsproxy(child_tgid, nsproxy);
	vns_put_nsproxy(nsproxy);
}

static void vns_registry_clear_all(void)
{
	struct vns_task *t;
	struct hlist_node *tmp;
	unsigned int bkt;

	hash_for_each_safe(vendor_kernel_registry.tasks, bkt, tmp, t, node) {
		hash_del(&t->node);
		if (t->nsproxy)
			vns_put_nsproxy(t->nsproxy);
		kfree(t);
	}
	vendor_kernel_registry.task_count = 0;
}

static void vns_resolve_symbols(void)
{
	vns_proc_alloc_inum_fn = (void *)shadow_hook_resolve("proc_alloc_inum");
	vns_proc_free_inum_fn = (void *)shadow_hook_resolve("proc_free_inum");
	vns_copy_mnt_ns_fn = (void *)shadow_hook_resolve("copy_mnt_ns");
	vns_put_mnt_ns_fn = (void *)shadow_hook_resolve("put_mnt_ns");
	vns_copy_net_ns_fn = (void *)shadow_hook_resolve("copy_net_ns");
	vns_put_net_ns_fn = (void *)shadow_hook_resolve("put_net");
}

int vendor_kernel_init(void)
{
	int ret;
	int hooked;

	if (vendor_kernel_enabled)
		return 0;

	hash_init(vendor_kernel_registry.tasks);
	spin_lock_init(&vendor_kernel_registry.lock);
	vendor_kernel_registry.task_count = 0;
	vendor_kernel_registry.stat_unshare = 0;
	vendor_kernel_registry.stat_setns = 0;
	vendor_kernel_registry.stat_clone = 0;

	vns_resolve_symbols();
	vns_compat_resolve(); /* [BUILD-COMPAT] resolve non-exported kernel symbols */
	if (!vns_compat_ready())
		return -ENOENT;
#ifdef CONFIG_CGROUPS
	/* [BUILD-COMPAT] init_cgroup_ns can't be used in a static initializer
	 * (not exported); patch vns_init_nsproxy at runtime once resolved. */
	if (vns_init_cgroup_ns_ptr)
		vns_init_nsproxy.cgroup_ns = vns_init_cgroup_ns_ptr;
#endif
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
	vns_init_nsproxy.ipc_ns = &init_ipc_ns;
#endif
	/* [BUILD-COMPAT] vendored init helpers do not create slab caches out of tree. */
	vns_uts_ns_init();
	vns_pid_ns_init();
	vns_user_ns_init();
	vns_nsfs_init();
#ifdef CONFIG_POSIX_MQUEUE
	ret = vns_mqueue_fs_init();
	if (ret)
		return ret;
#endif

	hooked = shadow_hook_install_all(vendor_kernel_core_hooks, "vendor_kernel");
	if (hooked < 0) {
#ifdef CONFIG_POSIX_MQUEUE
		vns_mqueue_fs_exit();
#endif
		return hooked;
	}

	vendor_kernel_enabled = true;
	LKM4CTR_INFO("vendor_kernel", "loaded (%d hook(s) installed)", hooked);
	return 0;
}

void vendor_kernel_exit(void)
{
	if (!vendor_kernel_enabled)
		return;
	vendor_kernel_enabled = false;
	shadow_hook_remove_all(vendor_kernel_core_hooks);
#ifdef CONFIG_POSIX_MQUEUE
	vns_mqueue_fs_exit();
#endif
	vns_registry_clear_all();
	LKM4CTR_INFO("vendor_kernel", "unloaded");
}
