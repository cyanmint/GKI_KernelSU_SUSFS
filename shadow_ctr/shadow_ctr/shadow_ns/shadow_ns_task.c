// SPDX-License-Identifier: GPL-2.0
#include "shadow_ns_internal.h"

struct shadow_task_group *shadow_ns_task_group_lookup(pid_t tgid)
{
	struct shadow_task_group *tg;

	if (tgid <= 0)
		return NULL;

	mutex_lock(&shadow_ns_tgid_lock);
	tg = xa_load(&shadow_ns_tgid_map, tgid);
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
	mutex_init(&tg->lock);
	ret = xa_err(xa_store(&shadow_ns_tgid_map, tgid, tg, GFP_KERNEL));
	if (ret) {
		mutex_destroy(&tg->lock);
		kfree(tg);
		mutex_unlock(&shadow_ns_tgid_lock);
		return ERR_PTR(ret);
	}

out_unlock:
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
		shadow_ns_task_group_free(tg);
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
		return ret;
	}

	mutex_lock(&child->lock);
	shadow_ns_drop_cur_array(child->cur);
	memcpy(child->cur, next, sizeof(child->cur));
	memset(next, 0, sizeof(next));
	shadow_ns_slot_replace(&child->cur[SHADOW_NS_TYPE_PID], pidns_for_child);
	mutex_unlock(&child->lock);

	if (pidns_for_child)
		shadow_ns_pidns_register(pidns_for_child->pid, child_tgid);
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
		return ret;
	if (!parent && !shadow_flags)
		return ret;

	child_tgid = shadow_ns_resolve_child_tgid((pid_t)ret);
	if (!child_tgid || child_tgid == task_tgid_nr(current))
		return ret;

	err = shadow_ns_install_child_state(child_tgid, parent, shadow_flags);
	if (err)
		pr_warn("shadow_ns: failed to install child state for tgid %d: %d\n",
			child_tgid, err);
	return ret;
}
