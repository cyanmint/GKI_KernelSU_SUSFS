// SPDX-License-Identifier: GPL-2.0
#include "shadow_sysvipc_internal.h"

/* Transparent syscall ownership registry: tgid -> svipc_tgid_owner. */
static DEFINE_MUTEX(svipc_tgid_lock);
static DEFINE_HASHTABLE(svipc_tgid_hash, SHADOW_SYSVIPC_TGID_HTBITS);

static int svipc_owned_ref_add_locked(struct list_head *owned,
				      struct svipc_resource *res)
{
	struct svipc_owned_ref *ref;

	ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (!ref)
		return -ENOMEM;
	ref->res = res;
	list_add(&ref->node, owned);
	return 0;
}

static int svipc_owned_ref_destroy_locked(struct list_head *owned, u32 type, u32 id)
{
	struct svipc_owned_ref *ref, *tmp;

	list_for_each_entry_safe(ref, tmp, owned, node) {
		if (ref->res->id == id && ref->res->type == type) {
			list_del(&ref->node);
			svipc_put(ref->res);
			kfree(ref);
			return 0;
		}
	}

	return -ENOENT;
}

static void svipc_owned_ref_release_all_locked(struct list_head *owned)
{
	struct svipc_owned_ref *ref, *tmp;

	list_for_each_entry_safe(ref, tmp, owned, node) {
		list_del(&ref->node);
		svipc_put(ref->res);
		kfree(ref);
	}
}

static struct svipc_tgid_owner *svipc_tgid_find_locked(pid_t tgid)
{
	struct svipc_tgid_owner *owner;

	hash_for_each_possible(svipc_tgid_hash, owner, node, (u32)tgid) {
		if (owner->tgid == tgid)
			return owner;
	}
	return NULL;
}

static bool svipc_tgid_alive(pid_t tgid)
{
	struct pid *pid;
	struct task_struct *task;
	bool alive = false;

	pid = find_get_pid(tgid);
	if (!pid)
		return false;

	task = get_pid_task(pid, PIDTYPE_TGID);
	if (task) {
		alive = true;
		put_task_struct(task);
	}
	put_pid(pid);
	return alive;
}

static void svipc_tgid_release_by_tgid(pid_t tgid)
{
	struct svipc_tgid_owner *owner;

	mutex_lock(&svipc_tgid_lock);
	owner = svipc_tgid_find_locked(tgid);
	if (!owner) {
		mutex_unlock(&svipc_tgid_lock);
		return;
	}
	hash_del(&owner->node);
	mutex_unlock(&svipc_tgid_lock);

	mutex_lock(&owner->lock);
	svipc_owned_ref_release_all_locked(&owner->owned);
	mutex_unlock(&owner->lock);
	mutex_destroy(&owner->lock);
	kfree(owner);
}

static void svipc_tgid_prune_empty(pid_t tgid)
{
	struct svipc_tgid_owner *owner;

	mutex_lock(&svipc_tgid_lock);
	owner = svipc_tgid_find_locked(tgid);
	if (!owner) {
		mutex_unlock(&svipc_tgid_lock);
		return;
	}

	mutex_lock(&owner->lock);
	if (!list_empty(&owner->owned)) {
		mutex_unlock(&owner->lock);
		mutex_unlock(&svipc_tgid_lock);
		return;
	}

	hash_del(&owner->node);
	mutex_unlock(&owner->lock);
	mutex_unlock(&svipc_tgid_lock);
	mutex_destroy(&owner->lock);
	kfree(owner);
}

void svipc_tgid_reap_dead(void)
{
	struct svipc_tgid_owner *owner;
	int bkt;
	pid_t dead_tgid;

again:
	dead_tgid = 0;
	mutex_lock(&svipc_tgid_lock);
	hash_for_each(svipc_tgid_hash, bkt, owner, node) {
		if (!svipc_tgid_alive(owner->tgid)) {
			dead_tgid = owner->tgid;
			break;
		}
	}
	mutex_unlock(&svipc_tgid_lock);

	if (dead_tgid) {
		svipc_tgid_release_by_tgid(dead_tgid);
		goto again;
	}
}

int svipc_tgid_own_current(struct svipc_resource *res)
{
	struct svipc_tgid_owner *owner;
	pid_t tgid = task_tgid_nr(current);
	bool created = false;
	int ret;

	mutex_lock(&svipc_tgid_lock);
	owner = svipc_tgid_find_locked(tgid);
	if (!owner) {
		owner = kzalloc(sizeof(*owner), GFP_KERNEL);
		if (!owner) {
			mutex_unlock(&svipc_tgid_lock);
			return -ENOMEM;
		}
		owner->tgid = tgid;
		INIT_LIST_HEAD(&owner->owned);
		mutex_init(&owner->lock);
		hash_add(svipc_tgid_hash, &owner->node, (u32)tgid);
		created = true;
	}

	mutex_lock(&owner->lock);
	mutex_unlock(&svipc_tgid_lock);

	ret = svipc_owned_ref_add_locked(&owner->owned, res);
	mutex_unlock(&owner->lock);
	if (ret && created)
		svipc_tgid_prune_empty(tgid);
	return ret;
}

int svipc_tgid_destroy_current(u32 type, u32 id)
{
	struct svipc_tgid_owner *owner;
	pid_t tgid = task_tgid_nr(current);
	int ret;

	mutex_lock(&svipc_tgid_lock);
	owner = svipc_tgid_find_locked(tgid);
	if (!owner) {
		mutex_unlock(&svipc_tgid_lock);
		return -ENOENT;
	}

	mutex_lock(&owner->lock);
	mutex_unlock(&svipc_tgid_lock);

	ret = svipc_owned_ref_destroy_locked(&owner->owned, type, id);
	mutex_unlock(&owner->lock);
	if (!ret)
		svipc_tgid_prune_empty(tgid);
	return ret;
}

void svipc_tgid_release_all(void)
{
	struct svipc_tgid_owner *owner;
	struct hlist_node *tmp;
	int bkt;

	mutex_lock(&svipc_tgid_lock);
	hash_for_each_safe(svipc_tgid_hash, bkt, tmp, owner, node) {
		hash_del(&owner->node);
		mutex_unlock(&svipc_tgid_lock);
		mutex_lock(&owner->lock);
		svipc_owned_ref_release_all_locked(&owner->owned);
		mutex_unlock(&owner->lock);
		mutex_destroy(&owner->lock);
		kfree(owner);
		mutex_lock(&svipc_tgid_lock);
	}
	mutex_unlock(&svipc_tgid_lock);
}
