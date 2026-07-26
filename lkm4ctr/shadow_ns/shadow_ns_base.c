// SPDX-License-Identifier: GPL-2.0
#include "shadow_ns_internal.h"

DEFINE_XARRAY_ALLOC1(shadow_ns_map);
DEFINE_MUTEX(shadow_ns_map_lock);
atomic_t shadow_ns_count = ATOMIC_INIT(0);

DEFINE_XARRAY(shadow_ns_tgid_map);
DEFINE_MUTEX(shadow_ns_tgid_lock);
struct delayed_work shadow_ns_reap_work;

unsigned long shadow_ns_type_to_clone_flag(u32 type)
{
	switch (type) {
	case SHADOW_NS_TYPE_UTS:
		return CLONE_NEWUTS;
	case SHADOW_NS_TYPE_IPC:
		return CLONE_NEWIPC;
	case SHADOW_NS_TYPE_MNT:
		return CLONE_NEWNS;
	case SHADOW_NS_TYPE_PID:
		return CLONE_NEWPID;
	case SHADOW_NS_TYPE_NET:
		return CLONE_NEWNET;
	case SHADOW_NS_TYPE_USER:
		return CLONE_NEWUSER;
	case SHADOW_NS_TYPE_CGROUP:
		return CLONE_NEWCGROUP;
	default:
		return 0;
	}
}

int shadow_ns_clone_flag_to_type(unsigned long flag)
{
	switch (flag) {
	case 0:
		return SHADOW_NS_TYPE_MAX;
	case CLONE_NEWUTS:
		return SHADOW_NS_TYPE_UTS;
	case CLONE_NEWIPC:
		return SHADOW_NS_TYPE_IPC;
	case CLONE_NEWNS:
		return SHADOW_NS_TYPE_MNT;
	case CLONE_NEWPID:
		return SHADOW_NS_TYPE_PID;
	case CLONE_NEWNET:
		return SHADOW_NS_TYPE_NET;
	case CLONE_NEWUSER:
		return SHADOW_NS_TYPE_USER;
	case CLONE_NEWCGROUP:
		return SHADOW_NS_TYPE_CGROUP;
	default:
		return -EINVAL;
	}
}

bool shadow_ns_requires_admin(unsigned long shadow_flags)
{
	return shadow_flags != 0;
}

#if defined(CONFIG_ARM64)
unsigned long shadow_ns_sys_arg0(const struct pt_regs *regs)
{
	return regs->regs[0];
}

unsigned long shadow_ns_sys_arg1(const struct pt_regs *regs)
{
	return regs->regs[1];
}

unsigned long shadow_ns_sys_arg2(const struct pt_regs *regs)
{
	return regs->regs[2];
}

void shadow_ns_sys_set_arg0(struct pt_regs *regs, unsigned long value)
{
	regs->regs[0] = value;
}

void shadow_ns_sys_set_arg1(struct pt_regs *regs, unsigned long value)
{
	regs->regs[1] = value;
}
#elif defined(CONFIG_X86_64)
unsigned long shadow_ns_sys_arg0(const struct pt_regs *regs)
{
	return regs->di;
}

unsigned long shadow_ns_sys_arg1(const struct pt_regs *regs)
{
	return regs->si;
}

unsigned long shadow_ns_sys_arg2(const struct pt_regs *regs)
{
	return regs->dx;
}

void shadow_ns_sys_set_arg0(struct pt_regs *regs, unsigned long value)
{
	regs->di = value;
}

void shadow_ns_sys_set_arg1(struct pt_regs *regs, unsigned long value)
{
	regs->si = value;
}
#else
#error "shadow_ns: unsupported architecture"
#endif

struct shadow_ns *shadow_ns_alloc(u32 type, u32 parent_id, struct shadow_ns *parent)
{
	struct shadow_ns *ns;
	u32 id;
	int ret;

	if (atomic_read(&shadow_ns_count) >= SHADOW_NS_MAX_NS)
		return ERR_PTR(-ENOSPC);

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return ERR_PTR(-ENOMEM);

	ns->type = type;
	ns->parent_id = parent_id;
	refcount_set(&ns->refcount, 1);

	if (type == SHADOW_NS_TYPE_UTS) {
		struct shadow_uts_priv *uts;

		uts = shadow_ns_uts_priv_alloc(parent ? parent->uts : NULL);
		if (IS_ERR(uts)) {
			ret = PTR_ERR(uts);
			kfree(ns);
			return ERR_PTR(ret);
		}
		ns->uts = uts;
	} else if (type == SHADOW_NS_TYPE_PID) {
		struct shadow_pidns_priv *pid;

		pid = shadow_ns_pidns_priv_alloc();
		if (IS_ERR(pid)) {
			ret = PTR_ERR(pid);
			kfree(ns);
			return ERR_PTR(ret);
		}
		ns->pid = pid;
	} else if (type == SHADOW_NS_TYPE_USER) {
		struct shadow_userns_priv *user;

		user = shadow_ns_userns_priv_alloc();
		if (IS_ERR(user)) {
			ret = PTR_ERR(user);
			kfree(ns);
			return ERR_PTR(ret);
		}
		ns->user = user;
	}

	mutex_lock(&shadow_ns_map_lock);
	ret = xa_alloc(&shadow_ns_map, &id, ns, XA_LIMIT(1, INT_MAX), GFP_KERNEL);
	mutex_unlock(&shadow_ns_map_lock);
	if (ret) {
		shadow_ns_uts_priv_free(ns->uts);
		shadow_ns_pidns_priv_free(ns->pid);
		shadow_ns_userns_priv_free(ns->user);
		kfree(ns);
		return ERR_PTR(ret);
	}

	ns->id = id;
	atomic_inc(&shadow_ns_count);
	return ns;
}

struct shadow_ns *shadow_ns_alloc_derived(u32 type, struct shadow_ns *parent)
{
	u32 parent_id = parent ? parent->id : 0;

	return shadow_ns_alloc(type, parent_id, parent);
}

struct shadow_ns *shadow_ns_grab(struct shadow_ns *ns)
{
	if (!ns)
		return NULL;
	if (!refcount_inc_not_zero(&ns->refcount))
		return NULL;
	return ns;
}

struct shadow_ns *shadow_ns_get(u32 id)
{
	struct shadow_ns *ns;

	if (!id)
		return NULL;

	mutex_lock(&shadow_ns_map_lock);
	ns = xa_load(&shadow_ns_map, id);
	if (ns && !refcount_inc_not_zero(&ns->refcount))
		ns = NULL;
	mutex_unlock(&shadow_ns_map_lock);
	return ns;
}

void shadow_ns_put(struct shadow_ns *ns)
{
	if (!ns)
		return;

	if (refcount_dec_and_test(&ns->refcount)) {
		mutex_lock(&shadow_ns_map_lock);
		xa_erase(&shadow_ns_map, ns->id);
		mutex_unlock(&shadow_ns_map_lock);
		atomic_dec(&shadow_ns_count);

		shadow_ns_uts_priv_free(ns->uts);
		shadow_ns_pidns_priv_free(ns->pid);
		shadow_ns_userns_priv_free(ns->user);
		kfree(ns);
	}
}

void shadow_ns_drop_cur_array(struct shadow_ns **cur)
{
	int type;

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		shadow_ns_put(cur[type]);
		cur[type] = NULL;
	}
}

void shadow_ns_slot_replace(struct shadow_ns **slot, struct shadow_ns *ns)
{
	if (*slot)
		shadow_ns_put(*slot);
	*slot = ns;
}

struct shadow_ns *shadow_ns_get_current(u32 type)
{
	struct shadow_task_group *tg;
	struct shadow_ns *ns;

	if (type >= SHADOW_NS_TYPE_MAX)
		return NULL;

	tg = shadow_ns_current_task_group(false);
	if (!tg)
		return NULL;

	mutex_lock(&tg->lock);
	ns = shadow_ns_grab(tg->cur[type]);
	mutex_unlock(&tg->lock);
	shadow_ns_task_group_put(tg);
	return ns;
}
