// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendored from kernel-common kernel/nsproxy.c (kernel version 6.1.124,
 * android14-6.1 branch). See lkm4ctr/vendor_ns/README.md for the vendoring
 * rules this file follows. Only the changes marked "RENAME", "DIAGFS
 * PLUMBING" or "STANDALONE COMPILE" below differ from the pristine kernel
 * source; everything else is intentionally kept close to the original.
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

/*
 * STANDALONE COMPILE: the real kernel stores the resulting nsproxy directly in
 * task_struct->nsproxy. vendor_ns cannot safely replace the builtin nsproxy, so
 * it keeps a side table keyed by thread-group id while preserving the upstream
 * clone/unshare control flow as closely as practical.
 */

static struct nsproxy *vns_create_nsproxy(void) /* RENAME */
{
	struct nsproxy *nsproxy = kzalloc(sizeof(*nsproxy), GFP_KERNEL); /* STANDALONE COMPILE */

	if (nsproxy)
		refcount_set(&nsproxy->count, 1);
	return nsproxy;
}

void vns_get_nsproxy(struct nsproxy *ns)
{
	if (ns)
		refcount_inc(&ns->count);
}

static void vns_free_nsproxy(struct nsproxy *ns) /* RENAME */
{
	if (!ns)
		return;
	if (ns->uts_ns)
		vns_free_uts_ns(ns->uts_ns); /* RENAME */
	if (ns->pid_ns_for_children)
		vns_put_pid_ns(ns->pid_ns_for_children); /* RENAME */
	kfree(ns); /* STANDALONE COMPILE */
}

void vns_put_nsproxy(struct nsproxy *ns)
{
	if (ns && refcount_dec_and_test(&ns->count))
		vns_free_nsproxy(ns); /* RENAME */
}

static struct user_namespace *vns_task_user_ns_locked(bool create) /* RENAME */
{
	struct vns_task *t;
	pid_t tgid = task_tgid_nr(current);

	hash_for_each_possible(vendor_ns_registry.tasks, t, node, tgid) {
		if (t->tgid == tgid)
			return t->user_ns;
	}
	return create ? vendor_ns_registry.root_user : NULL;
}

static struct vns_task *vns_task_find_locked(pid_t tgid)
{
	struct vns_task *t;

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
	t->user_ns = vendor_ns_registry.root_user;
	vns_get_nsproxy(t->nsproxy);
	vns_get_user_ns(t->user_ns);
	hash_add(vendor_ns_registry.tasks, &t->node, tgid);
	vendor_ns_registry.task_count++;
	return t;
}

static struct nsproxy *vns_current_nsproxy_locked(bool create)
{
	pid_t tgid = task_tgid_nr(current);
	struct vns_task *t = create ? vns_task_get_or_create_locked(tgid) : vns_task_find_locked(tgid);

	return t ? t->nsproxy : NULL;
}

struct nsproxy *vns_current_nsproxy(bool create)
{
	struct nsproxy *nsp;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(create);
	mutex_unlock(&vendor_ns_registry.lock);
	return nsp;
}

struct user_namespace *vns_current_user_ns(bool create)
{
	struct user_namespace *user_ns;

	mutex_lock(&vendor_ns_registry.lock);
	user_ns = vns_task_user_ns_locked(create);
	mutex_unlock(&vendor_ns_registry.lock);
	return user_ns;
}

static int vns_task_set_membership_locked(pid_t tgid, struct nsproxy *nsp,
		struct user_namespace *user_ns) /* RENAME */
{
	struct vns_task *t = vns_task_get_or_create_locked(tgid);
	struct nsproxy *old_nsproxy;
	struct user_namespace *old_user_ns;

	if (!t)
		return -ENOMEM;
	old_nsproxy = t->nsproxy;
	old_user_ns = t->user_ns;
	t->nsproxy = nsp;
	t->user_ns = user_ns;
	vns_get_nsproxy(nsp);
	vns_get_user_ns(user_ns);
	vns_put_nsproxy(old_nsproxy);
	vns_put_user_ns(old_user_ns);
	return 0;
}

struct nsproxy *vns_nsproxy_root(void)
{
	struct nsproxy *nsp = vns_create_nsproxy();

	if (!nsp)
		return NULL;
	nsp->uts_ns = vendor_ns_registry.root_uts;
	if (nsp->uts_ns)
		refcount_inc(&nsp->uts_ns->ns.count); /* STANDALONE COMPILE */
	nsp->pid_ns_for_children = vendor_ns_registry.root_pid;
	if (nsp->pid_ns_for_children)
		vns_get_pid_ns(nsp->pid_ns_for_children);
	if (current->nsproxy) {
		nsp->mnt_ns = current->nsproxy->mnt_ns;
		nsp->ipc_ns = current->nsproxy->ipc_ns;
		nsp->net_ns = current->nsproxy->net_ns;
		nsp->time_ns = current->nsproxy->time_ns;
		nsp->time_ns_for_children = current->nsproxy->time_ns_for_children;
		nsp->cgroup_ns = current->nsproxy->cgroup_ns;
	}
	return nsp;
}

static struct nsproxy *vns_create_new_namespaces(unsigned long flags,
	struct task_struct *tsk, struct user_namespace *user_ns,
	struct fs_struct *new_fs, struct user_namespace *old_user_ns,
	struct user_namespace **new_user_out) /* RENAME */
{
	struct nsproxy *new_nsp;

	new_nsp = vns_create_nsproxy();
	if (!new_nsp)
		return ERR_PTR(-ENOMEM);
	if (flags & CLONE_NEWUTS)
		new_nsp->uts_ns = vns_copy_utsname(flags, current_user_ns(), tsk->nsproxy ? tsk->nsproxy->uts_ns : vendor_ns_registry.root_uts);
	else {
		new_nsp->uts_ns = tsk->nsproxy ? tsk->nsproxy->uts_ns : vendor_ns_registry.root_uts;
		if (new_nsp->uts_ns)
			refcount_inc(&new_nsp->uts_ns->ns.count);
	}
	if (IS_ERR(new_nsp->uts_ns))
		goto out_ns;
	if (flags & CLONE_NEWPID)
		new_nsp->pid_ns_for_children = vns_copy_pid_ns(flags, current_user_ns(), tsk->nsproxy ? tsk->nsproxy->pid_ns_for_children : vendor_ns_registry.root_pid);
	else {
		new_nsp->pid_ns_for_children = tsk->nsproxy ? tsk->nsproxy->pid_ns_for_children : vendor_ns_registry.root_pid;
		if (new_nsp->pid_ns_for_children)
			vns_get_pid_ns(new_nsp->pid_ns_for_children);
	}
	if (IS_ERR(new_nsp->pid_ns_for_children))
		goto out_pid;
	new_nsp->mnt_ns = tsk->nsproxy ? tsk->nsproxy->mnt_ns : NULL;
	new_nsp->ipc_ns = tsk->nsproxy ? tsk->nsproxy->ipc_ns : NULL;
	new_nsp->net_ns = tsk->nsproxy ? tsk->nsproxy->net_ns : NULL;
	new_nsp->time_ns = tsk->nsproxy ? tsk->nsproxy->time_ns : NULL;
	new_nsp->time_ns_for_children = tsk->nsproxy ? tsk->nsproxy->time_ns_for_children : NULL;
	new_nsp->cgroup_ns = tsk->nsproxy ? tsk->nsproxy->cgroup_ns : NULL;
	if (flags & CLONE_NEWUSER)
		*new_user_out = vns_create_user_ns_from_parent(old_user_ns ? old_user_ns : vendor_ns_registry.root_user);
	else
		*new_user_out = old_user_ns ? vns_get_user_ns(old_user_ns) : vns_get_user_ns(vendor_ns_registry.root_user);
	if (!*new_user_out)
		goto out_user;
	return new_nsp;
out_user:
	vns_put_pid_ns(new_nsp->pid_ns_for_children);
out_pid:
	vns_free_uts_ns(new_nsp->uts_ns);
out_ns:
	kfree(new_nsp);
	return ERR_PTR(-ENOMEM);
}

int vns_do_unshare(unsigned long flags)
{
	struct nsproxy *old_ns, *new_ns;
	struct user_namespace *old_user_ns, *new_user_ns = NULL;
	int ret = 0;

	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_unshare++;
	old_ns = vns_current_nsproxy_locked(true);
	old_user_ns = vns_task_user_ns_locked(true);
	new_ns = vns_create_new_namespaces(flags & VENDOR_NS_ALL_FLAGS, current,
		current_user_ns(), current->fs, old_user_ns, &new_user_ns); /* RENAME */
	if (IS_ERR(new_ns)) {
		ret = PTR_ERR(new_ns);
		goto out;
	}
	ret = vns_task_set_membership_locked(task_tgid_nr(current), new_ns, new_user_ns);
	vns_put_nsproxy(new_ns);
	vns_put_user_ns(new_user_ns);
out:
	mutex_unlock(&vendor_ns_registry.lock);
	return ret;
}

int vns_do_setns_flags(unsigned long flags)
{
	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_setns++;
	vns_current_nsproxy_locked(true);
	mutex_unlock(&vendor_ns_registry.lock);
	return 0;
}

void vns_track_child(pid_t child_tgid)
{
	struct nsproxy *parent;
	struct user_namespace *user_ns;

	if (child_tgid <= 0)
		return;
	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_clone++;
	parent = vns_current_nsproxy_locked(true);
	user_ns = vns_task_user_ns_locked(true);
	if (parent && user_ns)
		vns_task_set_membership_locked(child_tgid, parent, user_ns);
	mutex_unlock(&vendor_ns_registry.lock);
}

void vns_track_child_flags(pid_t child_tgid, unsigned long flags)
{
	struct nsproxy *child;
	struct user_namespace *old_user_ns, *new_user_ns = NULL;
	struct nsproxy *old_ns;

	if (child_tgid <= 0)
		return;
	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_clone++;
	old_ns = vns_current_nsproxy_locked(true);
	old_user_ns = vns_task_user_ns_locked(true);
	child = vns_create_new_namespaces(flags & VENDOR_NS_ALL_FLAGS, current,
		current_user_ns(), current->fs, old_user_ns, &new_user_ns); /* RENAME */
	if (!IS_ERR(child)) {
		vns_task_set_membership_locked(child_tgid, child, new_user_ns);
		vns_put_nsproxy(child);
		vns_put_user_ns(new_user_ns);
	}
	mutex_unlock(&vendor_ns_registry.lock);
}

int vns_uts_set(const char *name, size_t len, bool domain)
{
	struct nsproxy *nsp;
	char *field;
	int ret = 0;

	if (len > __NEW_UTS_LEN)
		len = __NEW_UTS_LEN;
	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_uts_set++;
	nsp = vns_current_nsproxy_locked(true);
	if (!nsp || !nsp->uts_ns) {
		ret = -ENOMEM;
		goto out;
	}
	field = domain ? nsp->uts_ns->name.domainname : nsp->uts_ns->name.nodename;
	memset(field, 0, __NEW_UTS_LEN + 1);
	memcpy(field, name, len);
out:
	mutex_unlock(&vendor_ns_registry.lock);
	return ret;
}

int vns_uts_get(char *out, size_t outlen, bool domain)
{
	struct nsproxy *nsp;
	const char *field;
	int ret = -ENOENT;

	mutex_lock(&vendor_ns_registry.lock);
	vendor_ns_registry.stat_uts_get++;
	nsp = vns_current_nsproxy_locked(false);
	if (!nsp || !nsp->uts_ns)
		goto out;
	field = domain ? nsp->uts_ns->name.domainname : nsp->uts_ns->name.nodename;
	if (!field[0])
		goto out;
	strscpy(out, field, outlen);
	ret = 0;
out:
	mutex_unlock(&vendor_ns_registry.lock);
	return ret;
}

pid_t vns_pid_translate(pid_t real)
{
	struct nsproxy *nsp;
	pid_t out = real;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(false);
	if (nsp && nsp->pid_ns_for_children && nsp->pid_ns_for_children->level > 0) {
		if (idr_find(&nsp->pid_ns_for_children->idr, real) || real == 1)
			out = real;
		vendor_ns_registry.stat_pid_xlate++;
	}
	mutex_unlock(&vendor_ns_registry.lock);
	return out;
}

bool vns_pid_visible(pid_t real)
{
	struct nsproxy *nsp;
	bool visible = true;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(false);
	if (nsp && nsp->pid_ns_for_children && nsp->pid_ns_for_children->level > 0)
		visible = idr_find(&nsp->pid_ns_for_children->idr, real) || real == 1;
	mutex_unlock(&vendor_ns_registry.lock);
	return visible;
}

bool vns_in_child_pidns(void)
{
	struct nsproxy *nsp;
	bool child = false;

	mutex_lock(&vendor_ns_registry.lock);
	nsp = vns_current_nsproxy_locked(false);
	if (nsp && nsp->pid_ns_for_children && nsp->pid_ns_for_children->level > 0)
		child = true;
	mutex_unlock(&vendor_ns_registry.lock);
	return child;
}

uid_t vns_uid_translate(uid_t id)
{
	struct user_namespace *user_ns;
	uid_t out = id;

	mutex_lock(&vendor_ns_registry.lock);
	user_ns = vns_task_user_ns_locked(false);
	if (user_ns && user_ns->level > 0) {
		u32 mapped = vns_map_id_up(&user_ns->uid_map, id);
		if (mapped != (u32)-1)
			out = mapped;
		vendor_ns_registry.stat_uid_xlate++;
	}
	mutex_unlock(&vendor_ns_registry.lock);
	return out;
}

gid_t vns_gid_translate(gid_t id)
{
	struct user_namespace *user_ns;
	gid_t out = id;

	mutex_lock(&vendor_ns_registry.lock);
	user_ns = vns_task_user_ns_locked(false);
	if (user_ns && user_ns->level > 0) {
		u32 mapped = vns_map_id_up(&user_ns->gid_map, id);
		if (mapped != (u32)-1)
			out = mapped;
		vendor_ns_registry.stat_uid_xlate++;
	}
	mutex_unlock(&vendor_ns_registry.lock);
	return out;
}

void vns_task_purge_all(void)
{
	struct vns_task *t;
	struct hlist_node *tmp;
	int bkt;

	mutex_lock(&vendor_ns_registry.lock);
	hash_for_each_safe(vendor_ns_registry.tasks, bkt, tmp, t, node) {
		hash_del(&t->node);
		vns_put_nsproxy(t->nsproxy);
		vns_put_user_ns(t->user_ns);
		kfree(t);
	}
	vendor_ns_registry.task_count = 0;
	mutex_unlock(&vendor_ns_registry.lock);
}

/*
 * STANDALONE COMPILE: the real copy_namespaces()/switch_task_namespaces()
 * install path mutates task_struct->nsproxy and invokes time/perf/mount helpers.
 * vendor_ns instead mirrors only the namespace-copy bookkeeping through the side
 * table above, which is the portion its syscall hooks actually consume.
 */
