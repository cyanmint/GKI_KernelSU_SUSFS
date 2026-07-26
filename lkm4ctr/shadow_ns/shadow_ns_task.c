// SPDX-License-Identifier: GPL-2.0
#include "shadow_ns_internal.h"
#include "lkm4ctr_log.h"

static struct shadow_task_group *shadow_ns_task_group_grab(struct shadow_task_group *tg)
{
	if (!tg)
		return NULL;
	if (!refcount_inc_not_zero(&tg->refcount))
		return NULL;
	return tg;
}

struct shadow_task_group *shadow_ns_task_group_lookup(pid_t tgid)
{
	struct shadow_task_group *tg;

	if (tgid <= 0)
		return NULL;

	mutex_lock(&shadow_ns_tgid_lock);
	tg = shadow_ns_task_group_grab(xa_load(&shadow_ns_tgid_map, tgid));
	mutex_unlock(&shadow_ns_tgid_lock);
	return tg;
}

struct shadow_task_group *shadow_ns_task_group_get_or_create(pid_t tgid)
{
	struct shadow_task_group *tg;
	int ret;

	if (tgid <= 0)
		return ERR_PTR(-ESRCH);

	mutex_lock(&shadow_ns_tgid_lock);
	tg = xa_load(&shadow_ns_tgid_map, tgid);
	if (tg)
		goto out_unlock;

	tg = kzalloc(sizeof(*tg), GFP_KERNEL);
	if (!tg) {
		mutex_unlock(&shadow_ns_tgid_lock);
		return ERR_PTR(-ENOMEM);
	}

	tg->tgid = tgid;
	refcount_set(&tg->refcount, 1); /* map ownership */
	mutex_init(&tg->lock);
	ret = xa_err(xa_store(&shadow_ns_tgid_map, tgid, tg, GFP_KERNEL));
	if (ret) {
		mutex_destroy(&tg->lock);
		kfree(tg);
		mutex_unlock(&shadow_ns_tgid_lock);
		return ERR_PTR(ret);
	}

out_unlock:
	shadow_ns_task_group_grab(tg);
	mutex_unlock(&shadow_ns_tgid_lock);
	return tg;
}

struct shadow_task_group *shadow_ns_current_task_group(bool create)
{
	pid_t tgid = task_tgid_nr(current);

	if (create)
		return shadow_ns_task_group_get_or_create(tgid);
	return shadow_ns_task_group_lookup(tgid);
}

void shadow_ns_task_group_free(struct shadow_task_group *tg)
{
	if (!tg)
		return;

	if (tg->cur[SHADOW_NS_TYPE_PID])
		shadow_ns_pidns_unregister(tg->cur[SHADOW_NS_TYPE_PID]->pid, tg->tgid);
	shadow_ns_drop_cur_array(tg->cur);
	shadow_ns_put(tg->pending_pidns);
	tg->pending_pidns = NULL;
	mutex_destroy(&tg->lock);
	kfree(tg);
}

void shadow_ns_task_group_put(struct shadow_task_group *tg)
{
	if (!tg)
		return;
	if (refcount_dec_and_test(&tg->refcount))
		shadow_ns_task_group_free(tg);
}

bool shadow_ns_task_group_alive(pid_t tgid)
{
	struct pid *pid;
	struct task_struct *task;
	bool alive;

	pid = find_get_pid(tgid);
	if (!pid)
		return false;

	task = get_pid_task(pid, PIDTYPE_TGID);
	alive = task != NULL;
	if (task)
		put_task_struct(task);
	put_pid(pid);
	return alive;
}

void shadow_ns_reap_stale_task_groups(void)
{
	struct shadow_task_group *tg;
	unsigned long id = 0;

	for (;;) {
		mutex_lock(&shadow_ns_tgid_lock);
		tg = xa_find(&shadow_ns_tgid_map, &id, ULONG_MAX, XA_PRESENT);
		if (!tg) {
			mutex_unlock(&shadow_ns_tgid_lock);
			break;
		}
		if (shadow_ns_task_group_alive((pid_t)id)) {
			id++;
			mutex_unlock(&shadow_ns_tgid_lock);
			continue;
		}

		tg = xa_erase(&shadow_ns_tgid_map, id);
		mutex_unlock(&shadow_ns_tgid_lock);

		/*
		 * Safety net for shadow_ns_hook_exit_group()
		 * (shadow_ns_pid.c): a task that never reached our
		 * exit_group(2) hook (killed from outside, e.g. an external
		 * SIGKILL, or a kernel that lacks that syscall entirely)
		 * still needs its namespace's zap_pid_ns_processes() cascade
		 * to run if it happened to be that namespace's child
		 * reaper -- otherwise the rest of the namespace's tasks are
		 * silently orphaned instead of being torn down with it,
		 * same as a real pid_namespace would.
		 */
		if (tg && tg->cur[SHADOW_NS_TYPE_PID] &&
		    shadow_ns_pidns_is_child_reaper(tg->cur[SHADOW_NS_TYPE_PID]->pid,
						     tg->tgid))
			shadow_ns_pidns_zap(tg->cur[SHADOW_NS_TYPE_PID]->pid, tg->tgid);

		shadow_ns_task_group_put(tg);
	}
}

void shadow_ns_reap_workfn(struct work_struct *work)
{
	shadow_ns_reap_stale_task_groups();
	schedule_delayed_work(&shadow_ns_reap_work, SHADOW_NS_REAP_INTERVAL);
}

int shadow_ns_task_group_unshare_locked(struct shadow_task_group *tg,
					unsigned long shadow_flags)
{
	unsigned long generic_flags = shadow_flags & ~(unsigned long)CLONE_NEWPID;
	struct shadow_ns *created[SHADOW_NS_TYPE_MAX] = { };
	struct shadow_ns *new_pending_pidns = NULL;
	int type;
	int ret = 0;

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		if (type == SHADOW_NS_TYPE_PID)
			continue;
		if (!(generic_flags & shadow_ns_type_to_clone_flag(type)))
			continue;

		created[type] = shadow_ns_alloc_derived(type, tg->cur[type]);
		if (IS_ERR(created[type])) {
			ret = PTR_ERR(created[type]);
			created[type] = NULL;
			goto err_put;
		}
	}

	if (shadow_flags & CLONE_NEWPID) {
		new_pending_pidns = shadow_ns_alloc_derived(SHADOW_NS_TYPE_PID,
						    tg->cur[SHADOW_NS_TYPE_PID]);
		if (IS_ERR(new_pending_pidns)) {
			ret = PTR_ERR(new_pending_pidns);
			new_pending_pidns = NULL;
			goto err_put;
		}
	}

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		if (created[type])
			shadow_ns_slot_replace(&tg->cur[type], created[type]);
	}
	if (new_pending_pidns)
		shadow_ns_slot_replace(&tg->pending_pidns, new_pending_pidns);

	return 0;

err_put:
	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++)
		shadow_ns_put(created[type]);
	shadow_ns_put(new_pending_pidns);
	return ret;
}

int shadow_ns_prepare_child_cur_locked(struct shadow_task_group *parent,
				      unsigned long shadow_flags,
				      struct shadow_ns **next)
{
	int type;
	int ret;

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		struct shadow_ns *parent_ns = parent ? parent->cur[type] : NULL;

		if (type == SHADOW_NS_TYPE_PID)
			continue;

		if (shadow_flags & shadow_ns_type_to_clone_flag(type)) {
			next[type] = shadow_ns_alloc_derived(type, parent_ns);
			if (IS_ERR(next[type])) {
				ret = PTR_ERR(next[type]);
				next[type] = NULL;
				goto err_put;
			}
			continue;
		}

		next[type] = shadow_ns_grab(parent_ns);
	}

	return 0;

err_put:
	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		shadow_ns_put(next[type]);
		next[type] = NULL;
	}
	return ret;
}

int shadow_ns_child_pidns(struct shadow_task_group *parent,
			 unsigned long shadow_flags,
			 struct shadow_ns **out)
{
	struct shadow_ns *ns;

	*out = NULL;

	if (shadow_flags & CLONE_NEWPID) {
		ns = shadow_ns_alloc_derived(SHADOW_NS_TYPE_PID,
					     parent ? parent->cur[SHADOW_NS_TYPE_PID] : NULL);
		if (IS_ERR(ns))
			return PTR_ERR(ns);
		*out = ns;
		return 0;
	}

	if (parent) {
		struct shadow_ns *source = parent->pending_pidns ?
					    parent->pending_pidns :
					    parent->cur[SHADOW_NS_TYPE_PID];
		*out = shadow_ns_grab(source);
	}
	return 0;
}

int shadow_ns_install_child_state(pid_t child_tgid,
				 struct shadow_task_group *parent,
				 unsigned long shadow_flags)
{
	struct shadow_task_group *child;
	struct shadow_ns *next[SHADOW_NS_TYPE_MAX] = { };
	struct shadow_ns *pidns_for_child = NULL;
	int ret;
	int type;

	if (child_tgid <= 0)
		return -ESRCH;

	child = shadow_ns_task_group_get_or_create(child_tgid);
	if (IS_ERR(child))
		return PTR_ERR(child);

	if (parent)
		mutex_lock(&parent->lock);
	ret = shadow_ns_prepare_child_cur_locked(parent, shadow_flags, next);
	if (!ret)
		ret = shadow_ns_child_pidns(parent, shadow_flags, &pidns_for_child);
	if (parent)
		mutex_unlock(&parent->lock);
	if (ret) {
		for (type = 0; type < SHADOW_NS_TYPE_MAX; type++)
			shadow_ns_put(next[type]);
		shadow_ns_task_group_put(child);
		return ret;
	}

	mutex_lock(&child->lock);
	shadow_ns_drop_cur_array(child->cur);
	memcpy(child->cur, next, sizeof(child->cur));
	memset(next, 0, sizeof(next));
	shadow_ns_slot_replace(&child->cur[SHADOW_NS_TYPE_PID], pidns_for_child);
	mutex_unlock(&child->lock);

	if (pidns_for_child) {
		shadow_ns_pidns_register(pidns_for_child->pid, child_tgid);
		shadow_ns_pidns_init_task_ids(pidns_for_child->pid, child_tgid,
					      parent ? parent->tgid : 0);
	}
	shadow_ns_task_group_put(child);
	return 0;
}

static void shadow_join_cur(struct shadow_ns **cur, struct shadow_ns *ns)
{
	shadow_ns_slot_replace(&cur[ns->type], ns);
}

long shadow_ns_task_group_setns_by_id(int id, int flags)
{
	struct shadow_task_group *tg;
	struct shadow_ns *ns;
	int wanted_type;
	long ret;

	wanted_type = shadow_ns_clone_flag_to_type(flags);
	if (wanted_type < 0)
		return wanted_type;

	ns = shadow_ns_get(id);
	if (!ns)
		return -ENOENT;
	if (wanted_type != SHADOW_NS_TYPE_MAX && ns->type != wanted_type) {
		shadow_ns_put(ns);
		return -EINVAL;
	}
	if (!capable(CAP_SYS_ADMIN)) {
		shadow_ns_put(ns);
		return -EPERM;
	}

	tg = shadow_ns_current_task_group(true);
	if (IS_ERR(tg)) {
		shadow_ns_put(ns);
		return PTR_ERR(tg);
	}

	mutex_lock(&tg->lock);
	if (ns->type == SHADOW_NS_TYPE_PID)
		shadow_ns_slot_replace(&tg->pending_pidns, ns);
	else
		shadow_join_cur(tg->cur, ns);
	mutex_unlock(&tg->lock);
	ret = 0;
	shadow_ns_task_group_put(tg);
	return ret;
}

pid_t shadow_ns_resolve_child_tgid(pid_t pid)
{
	struct pid *pid_struct;
	struct task_struct *task;
	pid_t tgid = 0;

	if (pid <= 0)
		return 0;

	pid_struct = find_get_pid(pid);
	if (!pid_struct)
		return 0;
	task = get_pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	if (!task)
		return 0;

	tgid = task_tgid_nr(task);
	put_task_struct(task);
	return tgid;
}

long shadow_ns_clone_finalize(long ret, struct shadow_task_group *parent,
			      unsigned long shadow_flags)
{
	pid_t child_tgid;
	int err;

	if (ret <= 0)
		goto out_put_parent;
	if (!parent && !shadow_flags)
		goto out_put_parent;

	child_tgid = shadow_ns_resolve_child_tgid((pid_t)ret);
	if (!child_tgid || child_tgid == task_tgid_nr(current))
		goto out_put_parent;

	err = shadow_ns_install_child_state(child_tgid, parent, shadow_flags);
	if (err)
		LKM4CTR_WARN("shadow_ns", "failed to install child state for tgid %d: %d",
			     child_tgid, err);
out_put_parent:
	shadow_ns_task_group_put(parent);
	return ret;
}

struct shadow_ns *shadow_ns_pidns_for_tgid(pid_t rpid, bool for_children)
{
	struct shadow_task_group *tg;
	struct shadow_ns *ns;

	tg = shadow_ns_task_group_lookup(rpid);
	if (!tg)
		return NULL;

	mutex_lock(&tg->lock);
	ns = shadow_ns_grab(for_children && tg->pending_pidns ?
			     tg->pending_pidns : tg->cur[SHADOW_NS_TYPE_PID]);
	mutex_unlock(&tg->lock);
	shadow_ns_task_group_put(tg);
	return ns;
}

/*
 * shadow_ns_generic_for_tgid() - the simulated namespace of @type a given
 * real (host) tgid currently belongs to (tg->cur[type]), or NULL if that
 * task was never moved into one. Shared implementation behind
 * shadow_ns_userns_for_tgid() and shadow_ns_procfs.c's generic
 * /proc/<pid>/ns/{user,ipc,...} entry fabrication (shadow_ns_pid.c's own
 * ns/pid{,_for_children} entries stay on shadow_ns_pidns_for_tgid(), which
 * additionally understands pending_pidns / for_children semantics that do
 * not apply to any other namespace type). Returns a grabbed reference (the
 * caller must shadow_ns_put() it).
 */
struct shadow_ns *shadow_ns_generic_for_tgid(u32 type, pid_t rpid)
{
	struct shadow_task_group *tg;
	struct shadow_ns *ns;

	if (type >= SHADOW_NS_TYPE_MAX)
		return NULL;

	tg = shadow_ns_task_group_lookup(rpid);
	if (!tg)
		return NULL;

	mutex_lock(&tg->lock);
	ns = shadow_ns_grab(tg->cur[type]);
	mutex_unlock(&tg->lock);
	shadow_ns_task_group_put(tg);
	return ns;
}

/*
 * shadow_ns_userns_for_tgid() - the simulated USER namespace a given real
 * (host) tgid is currently a member of (tg->cur[USER]), or NULL if that
 * task was never moved into one (i.e. its getuid()/geteuid()/... are real,
 * unfaked passthroughs -- see shadow_ns_user.c). Used by shadow_ns_procfs.c
 * to decide whether an arbitrary target pid's /proc/<pid>/status Uid:/Gid:
 * lines need to be rewritten to all-zero, mirroring the same "creator's
 * uid/gid appear as 0" simulation getuid()/geteuid()/getgid()/getegid()
 * already apply to that task's syscalls.
 */
struct shadow_ns *shadow_ns_userns_for_tgid(pid_t rpid)
{
	return shadow_ns_generic_for_tgid(SHADOW_NS_TYPE_USER, rpid);
}

/*
 * shadow_ns_current_ipc_ns_id() - the id of the calling task's simulated
 * IPC namespace (shadow_ns's own bookkeeping-only CLONE_NEWIPC object, see
 * shadow_ns_module.c), or 0 if the caller was never moved into one (i.e. it
 * is still using the real/ambient IPC namespace).
 *
 * This is shadow_sysvipc's only coupling point with shadow_ns: both are
 * linked into the same lkm4ctr.ko (see ../Makefile), so a plain function
 * call resolves at link time without needing EXPORT_SYMBOL/symbol_get -- see
 * shadow_sysvipc_registry.c's svipc_current_ns_id(), which declares this
 * function's prototype itself rather than pulling in shadow_ns_internal.h's
 * much larger private surface.
 */
u32 shadow_ns_current_ipc_ns_id(void)
{
	struct shadow_ns *ns = shadow_ns_get_current(SHADOW_NS_TYPE_IPC);
	u32 id = 0;

	if (ns) {
		id = ns->id;
		shadow_ns_put(ns);
	}
	return id;
}
