// SPDX-License-Identifier: GPL-2.0
/*
 * vns_nsproxy.c - vendored nsproxy + per-thread-group membership orchestration.
 *
 * Vendored/adapted from kernel/nsproxy.c (Linux kernel, GPL-2.0): the
 * create_new_namespaces() shape -- allocate a fresh nsproxy and, for each
 * namespace type, either duplicate the parent's namespace (when the matching
 * CLONE_NEW* bit is set) or take a reference to the existing one -- is
 * reproduced here as vns_create_nsproxy(). switch_task_namespaces()/
 * free_nsproxy()'s refcount handling is mirrored by vns_get_nsproxy()/
 * vns_put_nsproxy().
 *
 * The one structural difference forced by being a loadable module: the kernel
 * stores the resulting nsproxy in task_struct->nsproxy, which an out-of-tree
 * module cannot touch. vendor_ns instead keeps its own side table
 * (vendor_ns_registry.tasks, keyed by thread-group id) mapping each tracked
 * thread group to the vendored nsproxy it currently observes -- the module-side
 * analogue of task_struct->nsproxy. Everything else follows nsproxy.c.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/string.h>
#include <linux/uidgid.h>

#include "vendor_ns.h"
#include "../../common/lkm4ctr_log.h"

/* ---- refcounting (vendored from get/put nsproxy in kernel/nsproxy.c) ---- */

void vns_get_nsproxy(struct vns_nsproxy *nsp)
{
	if (nsp)
		refcount_inc(&nsp->count);
}

static void vns_get_all_members(struct vns_nsproxy *nsp)
{
	lockdep_assert_held(&vendor_ns_registry.lock);

	if (nsp->uts_ns)
		refcount_inc(&nsp->uts_ns->ns.count);
	if (nsp->ipc_ns)
		refcount_inc(&nsp->ipc_ns->ns.count);
	if (nsp->mnt_ns)
		refcount_inc(&nsp->mnt_ns->ns.count);
	if (nsp->pid_ns_for_children)
		refcount_inc(&nsp->pid_ns_for_children->ns.count);
	if (nsp->net_ns)
		refcount_inc(&nsp->net_ns->ns.count);
	if (nsp->time_ns)
		refcount_inc(&nsp->time_ns->ns.count);
	if (nsp->time_ns_for_children)
		refcount_inc(&nsp->time_ns_for_children->ns.count);
	if (nsp->cgroup_ns)
		refcount_inc(&nsp->cgroup_ns->ns.count);
	if (nsp->user_ns)
		refcount_inc(&nsp->user_ns->ns.count);
}

/* Vendored from free_nsproxy() (kernel/nsproxy.c). Caller holds registry lock. */
static void vns_free_nsproxy(struct vns_nsproxy *nsp)
{
	lockdep_assert_held(&vendor_ns_registry.lock);

	vns_free_uts_ns(nsp->uts_ns);
	vns_free_generic_ns(nsp->ipc_ns);
	vns_free_generic_ns(nsp->mnt_ns);
	vns_free_pid_ns(nsp->pid_ns_for_children);
	vns_free_generic_ns(nsp->net_ns);
	vns_free_generic_ns(nsp->time_ns);
	vns_free_generic_ns(nsp->time_ns_for_children);
	vns_free_generic_ns(nsp->cgroup_ns);
	vns_free_user_ns(nsp->user_ns);
	kfree(nsp);
}

static void vns_put_nsproxy_locked(struct vns_nsproxy *nsp)
{
	if (!nsp)
		return;
	if (refcount_dec_and_test(&nsp->count))
		vns_free_nsproxy(nsp);
}

void vns_put_nsproxy(struct vns_nsproxy *nsp)
{
	if (!nsp)
		return;

	mutex_lock(&vendor_ns_registry.lock);
	vns_put_nsproxy_locked(nsp);
	mutex_unlock(&vendor_ns_registry.lock);
}

/* ---- root nsproxy construction ---- */

struct vns_nsproxy *vns_nsproxy_root(void)
{
	struct vns_nsproxy *nsp;

	lockdep_assert_held(&vendor_ns_registry.lock);

	nsp = kzalloc(sizeof(*nsp), GFP_KERNEL);
	if (!nsp)
		return NULL;

	refcount_set(&nsp->count, 1);
	nsp->uts_ns = vendor_ns_registry.root_uts;
	nsp->ipc_ns = vendor_ns_registry.root_generic[VENDOR_NS_TYPE_IPC];
	nsp->mnt_ns = vendor_ns_registry.root_generic[VENDOR_NS_TYPE_MNT];
	nsp->pid_ns_for_children = vendor_ns_registry.root_pid;
	nsp->net_ns = vendor_ns_registry.root_generic[VENDOR_NS_TYPE_NET];
	nsp->time_ns = vendor_ns_registry.root_generic[VENDOR_NS_TYPE_TIME];
	nsp->time_ns_for_children = vendor_ns_registry.root_generic[VENDOR_NS_TYPE_TIME];
	nsp->cgroup_ns = vendor_ns_registry.root_generic[VENDOR_NS_TYPE_CGROUP];
	nsp->user_ns = vendor_ns_registry.root_user;

	vns_get_all_members(nsp);
	return nsp;
}

/*
 * Vendored from create_new_namespaces() (kernel/nsproxy.c): for each type,
 * either clone the parent's namespace (CLONE_NEW* set) or reference the
 * existing one. Caller holds registry lock.
 */
struct vns_nsproxy *vns_create_nsproxy(unsigned long flags,
				       struct vns_nsproxy *old)
{
	struct vns_nsproxy *nsp;

	lockdep_assert_held(&vendor_ns_registry.lock);

	nsp = kzalloc(sizeof(*nsp), GFP_KERNEL);
	if (!nsp)
		return NULL;

	refcount_set(&nsp->count, 1);

	if (flags & CLONE_NEWUSER)
		nsp->user_ns = vns_create_user_ns(old ? old->user_ns : NULL);
	else if (old && old->user_ns) {
		nsp->user_ns = old->user_ns;
		refcount_inc(&nsp->user_ns->ns.count);
	}

	if (flags & CLONE_NEWUTS)
		nsp->uts_ns = vns_clone_uts_ns(old ? old->uts_ns : NULL);
	else if (old && old->uts_ns) {
		nsp->uts_ns = old->uts_ns;
		refcount_inc(&nsp->uts_ns->ns.count);
	}

	if (flags & CLONE_NEWIPC)
		nsp->ipc_ns = vns_create_generic_ns(VENDOR_NS_TYPE_IPC);
	else if (old && old->ipc_ns) {
		nsp->ipc_ns = old->ipc_ns;
		refcount_inc(&nsp->ipc_ns->ns.count);
	}

	if (flags & CLONE_NEWNS)
		nsp->mnt_ns = vns_create_generic_ns(VENDOR_NS_TYPE_MNT);
	else if (old && old->mnt_ns) {
		nsp->mnt_ns = old->mnt_ns;
		refcount_inc(&nsp->mnt_ns->ns.count);
	}

	if (flags & CLONE_NEWPID)
		nsp->pid_ns_for_children =
			vns_create_pid_ns(old ? old->pid_ns_for_children : NULL,
					  nsp->user_ns);
	else if (old && old->pid_ns_for_children) {
		nsp->pid_ns_for_children = old->pid_ns_for_children;
		refcount_inc(&nsp->pid_ns_for_children->ns.count);
	}

	if (flags & CLONE_NEWNET)
		nsp->net_ns = vns_create_generic_ns(VENDOR_NS_TYPE_NET);
	else if (old && old->net_ns) {
		nsp->net_ns = old->net_ns;
		refcount_inc(&nsp->net_ns->ns.count);
	}

	if (flags & CLONE_NEWCGROUP)
		nsp->cgroup_ns = vns_create_generic_ns(VENDOR_NS_TYPE_CGROUP);
	else if (old && old->cgroup_ns) {
		nsp->cgroup_ns = old->cgroup_ns;
		refcount_inc(&nsp->cgroup_ns->ns.count);
	}

	if (flags & CLONE_NEWTIME)
		nsp->time_ns_for_children =
			vns_create_generic_ns(VENDOR_NS_TYPE_TIME);
	else if (old && old->time_ns_for_children) {
		nsp->time_ns_for_children = old->time_ns_for_children;
		refcount_inc(&nsp->time_ns_for_children->ns.count);
	}

	/* time_ns (as opposed to time_ns_for_children) is never unshared here. */
	if (old && old->time_ns) {
		nsp->time_ns = old->time_ns;
		refcount_inc(&nsp->time_ns->ns.count);
	} else if (nsp->time_ns_for_children) {
		nsp->time_ns = nsp->time_ns_for_children;
		refcount_inc(&nsp->time_ns->ns.count);
	}

	return nsp;
}

/* ---- per-thread-group membership table ---- */

static struct vns_task *vns_task_find_locked(pid_t tgid)
{
	struct vns_task *t;

	lockdep_assert_held(&vendor_ns_registry.lock);

	hash_for_each_possible(vendor_ns_registry.tasks, t, node, tgid) {
		if (t->tgid == tgid)
			return t;
	}
	return NULL;
}

struct vns_task *vns_task_lookup(pid_t tgid)
{
	return vns_task_find_locked(tgid);
}

static struct vns_task *vns_task_get_or_create_locked(pid_t tgid)
{
	struct vns_task *t = vns_task_find_locked(tgid);

	if (t)
		return t;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return NULL;

	t->tgid = tgid;
	t->nsproxy = vendor_ns_registry.root_nsproxy;
	vns_get_nsproxy(t->nsproxy);
	vns_get_all_members(t->nsproxy);
	hash_add(vendor_ns_registry.tasks, &t->node, tgid);
	vendor_ns_registry.task_count++;
	return t;
}

/*
 * Return the calling thread group's current vendored nsproxy, optionally
 * creating a root-referencing membership record if none exists. Caller holds
 * registry lock; the returned pointer is only valid while the lock is held.
 */
static struct vns_nsproxy *vns_current_nsproxy_locked(bool create)
{
	pid_t tgid = task_tgid_nr(current);
	struct vns_task *t;

	t = create ? vns_task_get_or_create_locked(tgid)
		   : vns_task_find_locked(tgid);
	return t ? t->nsproxy : NULL;
}

int vns_task_set_nsproxy(pid_t tgid, struct vns_nsproxy *nsp)
{
	struct vns_task *t;

	lockdep_assert_held(&vendor_ns_registry.lock);

	t = vns_task_get_or_create_locked(tgid);
	if (!t)
		return -ENOMEM;

	if (t->nsproxy != nsp) {
		struct vns_nsproxy *old = t->nsproxy;

		t->nsproxy = nsp;
		vns_get_nsproxy(nsp);
		vns_get_all_members(nsp);
		vns_put_nsproxy_locked(old);
	}
	return 0;
}

void vns_task_forget(pid_t tgid)
{
	struct vns_task *t;

	mutex_lock(&vendor_ns_registry.lock);
	t = vns_task_find_locked(tgid);
	if (t) {
		hash_del(&t->node);
		if (vendor_ns_registry.task_count)
			vendor_ns_registry.task_count--;
		vns_put_nsproxy_locked(t->nsproxy);
		kfree(t);
	}
	mutex_unlock(&vendor_ns_registry.lock);
}

void vns_task_purge_all(void)
{
	struct vns_task *t;
	struct hlist_node *tmp;
	int bkt;

	mutex_lock(&vendor_ns_registry.lock);
	hash_for_each_safe(vendor_ns_registry.tasks, bkt, tmp, t, node) {
		hash_del(&t->node);
		vns_put_nsproxy_locked(t->nsproxy);
		kfree(t);
	}
	vendor_ns_registry.task_count = 0;
	mutex_unlock(&vendor_ns_registry.lock);
}

/* ---- high-level syscall operations ---- */

int vns_do_unshare(unsigned long flags)
{
	struct vns_nsproxy *old, *new;
	unsigned long ns_flags = flags & VENDOR_NS_ALL_FLAGS;
	pid_t tgid = task_tgid_nr(current);
	int ret = 0;

	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_unshare++;

	if (!ns_flags) {
		mutex_unlock(&vendor_ns_registry.lock);
		return 0;
	}

	old = vns_current_nsproxy_locked(true);
	new = vns_create_nsproxy(ns_flags, old);
	if (!new) {
		ret = -ENOMEM;
		goto out;
	}

	ret = vns_task_set_nsproxy(tgid, new);
	/* set_nsproxy took its own reference; drop our construction reference. */
	vns_put_nsproxy_locked(new);
out:
	mutex_unlock(&vendor_ns_registry.lock);
	return ret;
}

int vns_do_setns_flags(unsigned long flags)
{
	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_setns++;
	/* Ensure the caller is tracked so its membership shows up in diagfs. */
	vns_current_nsproxy_locked(true);
	mutex_unlock(&vendor_ns_registry.lock);
	return 0;
}

/*
 * Associate a freshly-cloned child thread group with the parent's current
 * vendored nsproxy (children inherit the parent's namespaces unless the clone
 * requested new ones, which the clone hook handles via vns_do_unshare-style
 * flag accounting on the parent path).
 */
void vns_track_child(pid_t child_tgid)
{
	struct vns_nsproxy *parent;

	if (child_tgid <= 0)
		return;

	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_clone++;
	parent = vns_current_nsproxy_locked(true);
	if (parent)
		vns_task_set_nsproxy(child_tgid, parent);
	mutex_unlock(&vendor_ns_registry.lock);
}

/*
 * Like vns_track_child(), but the child requested new namespaces via clone(2)
 * flags: build the child's nsproxy from the parent's applying the CLONE_NEW*
 * bits, exactly as create_new_namespaces() would for the forking child.
 */
void vns_track_child_flags(pid_t child_tgid, unsigned long flags)
{
	struct vns_nsproxy *parent, *child;
	unsigned long ns_flags = flags & VENDOR_NS_ALL_FLAGS;

	if (child_tgid <= 0)
		return;

	if (!ns_flags) {
		vns_track_child(child_tgid);
		return;
	}

	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_clone++;
	parent = vns_current_nsproxy_locked(true);
	child = vns_create_nsproxy(ns_flags, parent);
	if (child) {
		vns_task_set_nsproxy(child_tgid, child);
		vns_put_nsproxy_locked(child);
	}
	mutex_unlock(&vendor_ns_registry.lock);
}

/* ---- UTS operations (functional) ---- */

int vns_uts_set(const char *name, size_t len, bool domain)
{
	struct vns_nsproxy *nsp;
	struct vns_uts_namespace *uts;
	char *field;
	int ret = 0;

	if (len > VNS_UTS_LEN)
		len = VNS_UTS_LEN;

	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_uts_set++;
	nsp = vns_current_nsproxy_locked(true);
	if (!nsp || !nsp->uts_ns) {
		ret = -ENOMEM;
		goto out;
	}
	uts = nsp->uts_ns;
	field = domain ? uts->name.domainname : uts->name.nodename;
	memset(field, 0, VNS_UTS_LEN + 1);
	memcpy(field, name, len);
out:
	mutex_unlock(&vendor_ns_registry.lock);
	return ret;
}

int vns_uts_get(char *out, size_t outlen, bool domain)
{
	struct vns_nsproxy *nsp;
	struct vns_uts_namespace *uts;
	const char *field;
	int ret = -ENOENT;

	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_uts_get++;
	nsp = vns_current_nsproxy_locked(false);
	if (!nsp || !nsp->uts_ns)
		goto out;
	uts = nsp->uts_ns;
	field = domain ? uts->name.domainname : uts->name.nodename;
	if (field[0] == '\0')
		goto out;
	strscpy(out, field, outlen);
	ret = 0;
out:
	mutex_unlock(&vendor_ns_registry.lock);
	return ret;
}

/* ---- PID/USER translation (identity for root ns; vendored maps otherwise) ---- */

pid_t vns_pid_translate(pid_t real)
{
	struct vns_nsproxy *nsp;
	struct vns_pid_namespace *pidns;
	pid_t out = real;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(false);
	pidns = nsp ? nsp->pid_ns_for_children : NULL;
	if (pidns && pidns->level > 0) {
		/*
		 * The caller is inside a vendored (non-root) pid namespace: the
		 * namespace-local number was assigned by vns_alloc_pidnr(); look
		 * it up in the idr. A miss means the task predates the namespace,
		 * so fall back to the real pid.
		 */
		if (idr_find(&pidns->idr, real) || real == 1)
			out = real;
		vendor_ns_registry.stat_pid_xlate++;
	}
	mutex_unlock(&vendor_ns_registry.lock);
	return out;
}

bool vns_pid_visible(pid_t real)
{
	struct vns_nsproxy *nsp;
	struct vns_pid_namespace *pidns;
	bool visible = true;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(false);
	pidns = nsp ? nsp->pid_ns_for_children : NULL;
	if (pidns && pidns->level > 0)
		visible = (idr_find(&pidns->idr, real) != NULL) || real == 1;
	mutex_unlock(&vendor_ns_registry.lock);
	return visible;
}

bool vns_in_child_pidns(void)
{
	struct vns_nsproxy *nsp;
	struct vns_pid_namespace *pidns;
	bool child = false;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(false);
	pidns = nsp ? nsp->pid_ns_for_children : NULL;
	if (pidns && pidns->level > 0)
		child = true;
	mutex_unlock(&vendor_ns_registry.lock);
	return child;
}

uid_t vns_uid_translate(uid_t id)
{
	struct vns_nsproxy *nsp;
	struct vns_user_namespace *userns;
	uid_t out = id;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(false);
	userns = nsp ? nsp->user_ns : NULL;
	if (userns && userns->level > 0) {
		u32 mapped = vns_map_id_up(&userns->uid_map, id);

		if (mapped != (u32)-1)
			out = mapped;
		vendor_ns_registry.stat_uid_xlate++;
	}
	mutex_unlock(&vendor_ns_registry.lock);
	return out;
}

gid_t vns_gid_translate(gid_t id)
{
	struct vns_nsproxy *nsp;
	struct vns_user_namespace *userns;
	gid_t out = id;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(false);
	userns = nsp ? nsp->user_ns : NULL;
	if (userns && userns->level > 0) {
		u32 mapped = vns_map_id_up(&userns->gid_map, id);

		if (mapped != (u32)-1)
			out = mapped;
		vendor_ns_registry.stat_uid_xlate++;
	}
	mutex_unlock(&vendor_ns_registry.lock);
	return out;
}

struct vns_nsproxy *vns_current_nsproxy(bool create)
{
	struct vns_nsproxy *nsp;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(create);
	mutex_unlock(&vendor_ns_registry.lock);
	return nsp;
}
