// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns_base - generic ("shadow") namespace registry + syscall-hook module
 *
 * A standalone loadable kernel module that provides an independent,
 * reference-counted set of "shadow" namespace objects, the transparent
 * syscall-hook machinery that drives them, and a small plugin API that
 * per-type submodules (shadow_ns_uts.ko, shadow_ns_net.ko, ...) register with.
 *
 * What this base module simulates
 * -------------------------------
 * All seven namespace types (UTS/IPC/MNT/PID/NET/USER/CGROUP) get
 * reference-counted membership bookkeeping with parent/child lineage: stable
 * namespace identities and join semantics, but not real kernel-enforced
 * isolation. This works uniformly for every type regardless of which — if any
 * — per-type submodule is loaded (e.g. unshare(CLONE_NEWNET) keeps working as
 * bookkeeping even if shadow_ns_net.ko is never loaded).
 *
 * Per-type extensions (the plugin API, see common/shadow_ns_base.h)
 * -----------------------------------------------------------------
 * A submodule may call shadow_ns_base_register_type() to attach an opaque
 * per-namespace payload (priv_alloc/priv_free) and advertise whether it adds
 * genuine functional behaviour (->real_support). Today only shadow_ns_uts.ko
 * does the latter (real nodename/domainname storage plus the
 * sethostname/setdomainname/uname syscall hooks, which live in *that* module,
 * not here). If no submodule is registered for a type, its payload stays NULL
 * and behaviour is exactly the bookkeeping-only default.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <linux/refcount.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/workqueue.h>

#include "shadow_hook.h"
#include "shadow_ns_base.h"
#include "include/uapi/shadow_ns.h"

#define SHADOW_NS_VERSION		"2.0"
#define SHADOW_NS_MAX_NS		65536
#define SHADOW_NS_SHADOW_CLONE_FLAGS	(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | \
				 CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUSER | \
				 CLONE_NEWCGROUP)

/*
 * struct shadow_ns - a single shadow namespace object.
 * @id:        stable identifier handed to userspace (xarray index)
 * @type:      enum shadow_ns_type
 * @parent_id: id of the namespace this was cloned from, 0 if none
 * @refcount:  dropped by every holder that references this object
 * @type_priv: opaque per-type payload owned by the registered submodule for
 *             @type (allocated by ops->priv_alloc, freed by ops->priv_free),
 *             or NULL when no submodule is registered / the type keeps no
 *             payload. Never dereferenced by the base module itself.
 */
struct shadow_ns {
	u32			id;
	u32			type;
	u32			parent_id;
	refcount_t		refcount;
	void			*type_priv;
};

/*
 * struct shadow_task_group - transparent syscall-facing state keyed by TGID.
 * @tgid: thread-group id (what userspace sees as PID for a process)
 * @cur:  current shadow namespace of each type for this task group
 * @lock: serialises updates to @cur and UTS payloads
 */
struct shadow_task_group {
	pid_t				 tgid;
	struct shadow_ns		*cur[SHADOW_NS_TYPE_MAX];
	struct mutex			 lock;
};

/* Global registry of shadow namespaces: id -> struct shadow_ns *. */
static DEFINE_XARRAY_ALLOC1(shadow_ns_map);
static DEFINE_MUTEX(shadow_ns_map_lock);
static atomic_t shadow_ns_count = ATOMIC_INIT(0);

/* Transparent task-group registry: tgid -> struct shadow_task_group *. */
static DEFINE_XARRAY(shadow_ns_tgid_map);
static DEFINE_MUTEX(shadow_ns_tgid_lock);
static struct delayed_work shadow_ns_reap_work;

static long (*real_sys_unshare)(const struct pt_regs *regs);
static long (*real_sys_setns)(const struct pt_regs *regs);
static long (*real_sys_clone)(const struct pt_regs *regs);
static long (*real_sys_clone3)(const struct pt_regs *regs);
static long (*real_sys_fork)(const struct pt_regs *regs);
static long (*real_sys_vfork)(const struct pt_regs *regs);

#define SHADOW_NS_REAP_INTERVAL	(30 * HZ)

static const char * const shadow_ns_unshare_names[] = {
	"__arm64_sys_unshare",
	"__x64_sys_unshare",
	"sys_unshare",
	NULL,
};
static const char * const shadow_ns_setns_names[] = {
	"__arm64_sys_setns",
	"__x64_sys_setns",
	"sys_setns",
	NULL,
};
static const char * const shadow_ns_clone_names[] = {
	"__arm64_sys_clone",
	"__x64_sys_clone",
	"sys_clone",
	NULL,
};
static const char * const shadow_ns_clone3_names[] = {
	"__arm64_sys_clone3",
	"__x64_sys_clone3",
	"sys_clone3",
	NULL,
};
static const char * const shadow_ns_fork_names[] = {
	"__arm64_sys_fork",
	"__x64_sys_fork",
	"sys_fork",
	NULL,
};
static const char * const shadow_ns_vfork_names[] = {
	"__arm64_sys_vfork",
	"__x64_sys_vfork",
	"sys_vfork",
	NULL,
};

static long shadow_ns_hook_unshare(const struct pt_regs *regs);
static long shadow_ns_hook_setns(const struct pt_regs *regs);
static long shadow_ns_hook_clone(const struct pt_regs *regs);
static long shadow_ns_hook_clone3(const struct pt_regs *regs);
static long shadow_ns_hook_fork(const struct pt_regs *regs);
static long shadow_ns_hook_vfork(const struct pt_regs *regs);

static struct shadow_hook shadow_ns_unshare_hook =
	SHADOW_HOOK(shadow_ns_unshare_names, shadow_ns_hook_unshare,
		    &real_sys_unshare);
static struct shadow_hook shadow_ns_setns_hook =
	SHADOW_HOOK(shadow_ns_setns_names, shadow_ns_hook_setns, &real_sys_setns);
static struct shadow_hook shadow_ns_clone_hook =
	SHADOW_HOOK(shadow_ns_clone_names, shadow_ns_hook_clone, &real_sys_clone);
static struct shadow_hook shadow_ns_clone3_hook =
	SHADOW_HOOK(shadow_ns_clone3_names, shadow_ns_hook_clone3, &real_sys_clone3);
static struct shadow_hook shadow_ns_fork_hook =
	SHADOW_HOOK(shadow_ns_fork_names, shadow_ns_hook_fork, &real_sys_fork);
static struct shadow_hook shadow_ns_vfork_hook =
	SHADOW_HOOK(shadow_ns_vfork_names, shadow_ns_hook_vfork, &real_sys_vfork);

static struct shadow_hook *shadow_ns_hooks[] = {
	&shadow_ns_unshare_hook,
	&shadow_ns_setns_hook,
	&shadow_ns_clone_hook,
	&shadow_ns_clone3_hook,
	&shadow_ns_fork_hook,
	&shadow_ns_vfork_hook,
	NULL,
};

/*
 * --- per-type plugin table -------------------------------------------------
 *
 * shadow_ns_type_ops_tbl[type] points at the ops a submodule registered for
 * @type, or NULL. Protected by shadow_ns_type_lock. The ops struct is a
 * static const in the submodule, so it stays valid as long as that module is
 * loaded.
 *
 * Callback invocation (priv_alloc/priv_free) must not race with the
 * submodule unloading: we take a *per-call* try_module_get(ops->owner) around
 * each invocation (see shadow_ns_type_ops_tryget()). A held reference blocks
 * the submodule's module_exit() (hence its shadow_ns_base_unregister_type())
 * from running until the callback returns, so the function pointer can never
 * be freed mid-call. We deliberately do NOT pin ops->owner for the whole
 * registration lifetime: that would make the submodule permanently
 * un-unloadable (its own module_exit is the only unregister trigger, and it
 * cannot run while pinned), defeating the goal of independently loadable *and*
 * unloadable modules. The reverse edge — shadow_ns_base cannot unload while a
 * submodule is loaded — is already guaranteed automatically by the module
 * loader, because the submodule uses shadow_ns_base's exported symbols.
 */
static const struct shadow_ns_type_ops *shadow_ns_type_ops_tbl[SHADOW_NS_TYPE_MAX];
static DEFINE_MUTEX(shadow_ns_type_lock);

/* Presence check only (no module reference taken). */
static const struct shadow_ns_type_ops *shadow_ns_type_ops_get(u32 type)
{
	const struct shadow_ns_type_ops *ops;

	if (type >= SHADOW_NS_TYPE_MAX)
		return NULL;

	mutex_lock(&shadow_ns_type_lock);
	ops = shadow_ns_type_ops_tbl[type];
	mutex_unlock(&shadow_ns_type_lock);
	return ops;
}

/*
 * Fetch the ops for @type and pin its owning module for the duration of a
 * single callback. Returns NULL if no ops are registered or the owner is
 * being unloaded. Pair every successful call with shadow_ns_type_ops_putmod().
 */
static const struct shadow_ns_type_ops *shadow_ns_type_ops_tryget(u32 type)
{
	const struct shadow_ns_type_ops *ops;

	if (type >= SHADOW_NS_TYPE_MAX)
		return NULL;

	mutex_lock(&shadow_ns_type_lock);
	ops = shadow_ns_type_ops_tbl[type];
	if (ops && ops->owner && !try_module_get(ops->owner))
		ops = NULL;
	mutex_unlock(&shadow_ns_type_lock);
	return ops;
}

static void shadow_ns_type_ops_putmod(const struct shadow_ns_type_ops *ops)
{
	if (ops && ops->owner)
		module_put(ops->owner);
}

static unsigned long shadow_ns_type_to_clone_flag(u32 type)
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

static int shadow_ns_clone_flag_to_type(unsigned long flag)
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

static bool shadow_ns_requires_admin(unsigned long shadow_flags)
{
	return shadow_flags != 0;
}

#if defined(CONFIG_ARM64)
static unsigned long shadow_ns_sys_arg0(const struct pt_regs *regs)
{
	return regs->regs[0];
}

static unsigned long shadow_ns_sys_arg1(const struct pt_regs *regs)
{
	return regs->regs[1];
}

static void shadow_ns_sys_set_arg0(struct pt_regs *regs, unsigned long value)
{
	regs->regs[0] = value;
}
#elif defined(CONFIG_X86_64)
static unsigned long shadow_ns_sys_arg0(const struct pt_regs *regs)
{
	return regs->di;
}

static unsigned long shadow_ns_sys_arg1(const struct pt_regs *regs)
{
	return regs->si;
}

static void shadow_ns_sys_set_arg0(struct pt_regs *regs, unsigned long value)
{
	regs->di = value;
}
#else
#error "shadow_ns_base: unsupported architecture"
#endif

/*
 * Allocate a new shadow namespace with refcount 1. Caller owns the reference.
 * @parent_id/@parent_priv describe the namespace this one derives from (0/NULL
 * for a fresh/root instance) and are handed to the registered type's
 * ->priv_alloc, if any.
 */
static struct shadow_ns *shadow_ns_alloc(u32 type, u32 parent_id,
					void *parent_priv)
{
	const struct shadow_ns_type_ops *ops;
	struct shadow_ns *ns;
	void *priv = NULL;
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

	ops = shadow_ns_type_ops_tryget(type);
	if (ops && ops->priv_alloc) {
		priv = ops->priv_alloc(parent_id, parent_priv);
		if (IS_ERR(priv)) {
			ret = PTR_ERR(priv);
			shadow_ns_type_ops_putmod(ops);
			kfree(ns);
			return ERR_PTR(ret);
		}
		ns->type_priv = priv;
	}

	mutex_lock(&shadow_ns_map_lock);
	/* Reserve id in [1, INT_MAX]; 0 is reserved to mean "no namespace". */
	ret = xa_alloc(&shadow_ns_map, &id, ns, XA_LIMIT(1, INT_MAX), GFP_KERNEL);
	mutex_unlock(&shadow_ns_map_lock);
	if (ret) {
		if (priv && ops && ops->priv_free)
			ops->priv_free(priv);
		shadow_ns_type_ops_putmod(ops);
		kfree(ns);
		return ERR_PTR(ret);
	}

	shadow_ns_type_ops_putmod(ops);
	ns->id = id;
	atomic_inc(&shadow_ns_count);
	return ns;
}

static struct shadow_ns *shadow_ns_alloc_derived(u32 type, struct shadow_ns *parent)
{
	void *parent_priv = NULL;
	u32 parent_id = 0;

	if (parent) {
		parent_id = parent->id;
		parent_priv = parent->type_priv;
	}

	return shadow_ns_alloc(type, parent_id, parent_priv);
}

static struct shadow_ns *shadow_ns_grab(struct shadow_ns *ns)
{
	if (!ns)
		return NULL;
	if (!refcount_inc_not_zero(&ns->refcount))
		return NULL;
	return ns;
}

static struct shadow_ns *shadow_ns_get(u32 id)
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

static void shadow_ns_put(struct shadow_ns *ns)
{
	if (!ns)
		return;

	if (refcount_dec_and_test(&ns->refcount)) {
		const struct shadow_ns_type_ops *ops;

		mutex_lock(&shadow_ns_map_lock);
		xa_erase(&shadow_ns_map, ns->id);
		mutex_unlock(&shadow_ns_map_lock);
		atomic_dec(&shadow_ns_count);

		if (ns->type_priv) {
			ops = shadow_ns_type_ops_tryget(ns->type);
			if (ops && ops->priv_free)
				ops->priv_free(ns->type_priv);
			shadow_ns_type_ops_putmod(ops);
		}
		kfree(ns);
	}
}

static void shadow_ns_drop_cur_array(struct shadow_ns **cur)
{
	int type;

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		shadow_ns_put(cur[type]);
		cur[type] = NULL;
	}
}

static void shadow_ns_slot_replace(struct shadow_ns **slot, struct shadow_ns *ns)
{
	if (*slot)
		shadow_ns_put(*slot);
	*slot = ns;
}

static struct shadow_task_group *shadow_ns_task_group_lookup(pid_t tgid)
{
	struct shadow_task_group *tg;

	if (tgid <= 0)
		return NULL;

	mutex_lock(&shadow_ns_tgid_lock);
	tg = xa_load(&shadow_ns_tgid_map, tgid);
	mutex_unlock(&shadow_ns_tgid_lock);
	return tg;
}

static struct shadow_task_group *shadow_ns_task_group_get_or_create(pid_t tgid)
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

static struct shadow_task_group *shadow_ns_current_task_group(bool create)
{
	pid_t tgid = task_tgid_nr(current);

	if (create)
		return shadow_ns_task_group_get_or_create(tgid);
	return shadow_ns_task_group_lookup(tgid);
}

static void shadow_ns_task_group_free(struct shadow_task_group *tg)
{
	if (!tg)
		return;

	shadow_ns_drop_cur_array(tg->cur);
	mutex_destroy(&tg->lock);
	kfree(tg);
}

static bool shadow_ns_task_group_alive(pid_t tgid)
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

static void shadow_ns_reap_stale_task_groups(void)
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

static void shadow_ns_reap_workfn(struct work_struct *work)
{
	shadow_ns_reap_stale_task_groups();
	schedule_delayed_work(&shadow_ns_reap_work, SHADOW_NS_REAP_INTERVAL);
}

static int shadow_ns_task_group_unshare_locked(struct shadow_task_group *tg,
					      unsigned long shadow_flags)
{
	struct shadow_ns *created[SHADOW_NS_TYPE_MAX] = { };
	int type;
	int ret = 0;

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		if (!(shadow_flags & shadow_ns_type_to_clone_flag(type)))
			continue;

		created[type] = shadow_ns_alloc_derived(type, tg->cur[type]);
		if (IS_ERR(created[type])) {
			ret = PTR_ERR(created[type]);
			created[type] = NULL;
			goto err_put;
		}
	}

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		if (created[type])
			shadow_ns_slot_replace(&tg->cur[type], created[type]);
	}

	return 0;

err_put:
	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++)
		shadow_ns_put(created[type]);
	return ret;
}

static int shadow_ns_prepare_child_cur_locked(struct shadow_task_group *parent,
					     unsigned long shadow_flags,
					     struct shadow_ns **next)
{
	int type;
	int ret;

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		struct shadow_ns *parent_ns = parent ? parent->cur[type] : NULL;

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

static int shadow_ns_install_child_state(pid_t child_tgid,
					struct shadow_task_group *parent,
					unsigned long shadow_flags)
{
	struct shadow_task_group *child;
	struct shadow_ns *next[SHADOW_NS_TYPE_MAX] = { };
	int ret;

	if (child_tgid <= 0)
		return -ESRCH;

	child = shadow_ns_task_group_get_or_create(child_tgid);
	if (IS_ERR(child))
		return PTR_ERR(child);

	if (parent)
		mutex_lock(&parent->lock);
	ret = shadow_ns_prepare_child_cur_locked(parent, shadow_flags, next);
	if (parent)
		mutex_unlock(&parent->lock);
	if (ret)
		return ret;

	mutex_lock(&child->lock);
	shadow_ns_drop_cur_array(child->cur);
	memcpy(child->cur, next, sizeof(child->cur));
	memset(next, 0, sizeof(next));
	mutex_unlock(&child->lock);
	return 0;
}

/* Join @ns as the current namespace of its type (consumes a ref). */
static void shadow_join_cur(struct shadow_ns **cur, struct shadow_ns *ns)
{
	shadow_ns_slot_replace(&cur[ns->type], ns);
}

static int shadow_ns_join_task_group_ns(struct shadow_task_group *tg,
				       struct shadow_ns *ns)
{
	if (!tg || !ns)
		return -EINVAL;

	mutex_lock(&tg->lock);
	shadow_join_cur(tg->cur, ns);
	mutex_unlock(&tg->lock);
	return 0;
}

static long shadow_ns_task_group_setns_by_id(int id, int flags)
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

	ret = shadow_ns_join_task_group_ns(tg, ns);
	return ret;
}

static pid_t shadow_ns_resolve_child_tgid(pid_t pid)
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

static long shadow_ns_clone_finalize(long ret, struct shadow_task_group *parent,
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

static long shadow_ns_hook_unshare(const struct pt_regs *regs)
{
	struct shadow_task_group *tg;
	struct pt_regs regs_copy;
	unsigned long flags = shadow_ns_sys_arg0(regs);
	unsigned long shadow_flags = flags & SHADOW_NS_SHADOW_CLONE_FLAGS;
	unsigned long native_flags = flags & ~SHADOW_NS_SHADOW_CLONE_FLAGS;
	long ret = 0;

	if (!shadow_flags)
		return real_sys_unshare(regs);
	if (shadow_ns_requires_admin(shadow_flags) && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (native_flags || !shadow_flags) {
		regs_copy = *regs;
		shadow_ns_sys_set_arg0(&regs_copy, native_flags);
		ret = real_sys_unshare(&regs_copy);
		if (ret)
			return ret;
	}

	tg = shadow_ns_current_task_group(true);
	if (IS_ERR(tg))
		return PTR_ERR(tg);

	mutex_lock(&tg->lock);
	ret = shadow_ns_task_group_unshare_locked(tg, shadow_flags);
	mutex_unlock(&tg->lock);
	return ret;
}

static long shadow_ns_hook_setns(const struct pt_regs *regs)
{
	int fd = (int)shadow_ns_sys_arg0(regs);
	int flags = (int)shadow_ns_sys_arg1(regs);
	long ret = real_sys_setns(regs);
	long shadow_ret;

	if (ret != -EINVAL && ret != -ENOTTY)
		return ret;

	/*
	 * Best-effort private encoding for shadow-only joins: use the numeric
	 * namespace id directly in place of @fd when there is no real nsfs fd.
	 */
	shadow_ret = shadow_ns_task_group_setns_by_id(fd, flags);
	if (!shadow_ret)
		return 0;

	return ret;
}

static long shadow_ns_hook_clone(const struct pt_regs *regs)
{
	struct shadow_task_group *parent = shadow_ns_current_task_group(false);
	struct pt_regs regs_copy = *regs;
	unsigned long flags = shadow_ns_sys_arg0(regs);
	unsigned long shadow_flags = flags & SHADOW_NS_SHADOW_CLONE_FLAGS;
	long ret;

	if (shadow_flags && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (shadow_flags)
		shadow_ns_sys_set_arg0(&regs_copy, flags & ~SHADOW_NS_SHADOW_CLONE_FLAGS);
	ret = real_sys_clone(&regs_copy);
	return shadow_ns_clone_finalize(ret, parent, shadow_flags);
}

static long shadow_ns_hook_clone3(const struct pt_regs *regs)
{
	struct shadow_task_group *parent = shadow_ns_current_task_group(false);
	void __user *uargs = (void __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	u64 orig_flags;
	u64 native_flags;
	unsigned long shadow_flags;
	long ret;
	bool patched = false;

	if (!uargs || copy_from_user(&orig_flags, uargs, sizeof(orig_flags)))
		return real_sys_clone3(regs);

	shadow_flags = (unsigned long)(orig_flags & SHADOW_NS_SHADOW_CLONE_FLAGS);
	if (shadow_flags && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	native_flags = orig_flags & ~((u64)SHADOW_NS_SHADOW_CLONE_FLAGS);
	if (shadow_flags) {
		if (copy_to_user(uargs, &native_flags, sizeof(native_flags)))
			return -EFAULT;
		patched = true;
	}

	ret = real_sys_clone3(regs);

	if (patched && copy_to_user(uargs, &orig_flags, sizeof(orig_flags)))
		pr_warn("shadow_ns: failed to restore clone3 flags for current task\n");

	return shadow_ns_clone_finalize(ret, parent, shadow_flags);
}

static long shadow_ns_hook_fork(const struct pt_regs *regs)
{
	struct shadow_task_group *parent = shadow_ns_current_task_group(false);
	long ret = real_sys_fork(regs);

	return shadow_ns_clone_finalize(ret, parent, 0);
}

static long shadow_ns_hook_vfork(const struct pt_regs *regs)
{
	struct shadow_task_group *parent = shadow_ns_current_task_group(false);
	long ret = real_sys_vfork(regs);

	return shadow_ns_clone_finalize(ret, parent, 0);
}

/*
 * --- exported plugin API (see common/shadow_ns_base.h) ---------------------
 */

/* Set once shadow_ns_exit() starts, to refuse late registrations. */
static bool shadow_ns_base_exiting;

int shadow_ns_base_register_type(u32 type, const struct shadow_ns_type_ops *ops)
{
	struct shadow_ns *ns;
	unsigned long id;

	if (type >= SHADOW_NS_TYPE_MAX || !ops)
		return -EINVAL;

	mutex_lock(&shadow_ns_type_lock);
	if (shadow_ns_base_exiting) {
		mutex_unlock(&shadow_ns_type_lock);
		return -ENODEV;
	}
	if (shadow_ns_type_ops_tbl[type]) {
		mutex_unlock(&shadow_ns_type_lock);
		return -EBUSY;
	}
	shadow_ns_type_ops_tbl[type] = ops;
	mutex_unlock(&shadow_ns_type_lock);

	/*
	 * Retroactively attach payloads to any pre-existing namespaces of this
	 * type that were created (as bookkeeping) before the submodule loaded,
	 * so e.g. a UTS namespace made via unshare() before shadow_ns_uts.ko
	 * was inserted still gains a working payload. Parent inheritance is not
	 * reconstructed here (a fresh/empty payload is allocated); normal use
	 * loads the submodule before creating namespaces of its type.
	 */
	if (ops->priv_alloc) {
		mutex_lock(&shadow_ns_map_lock);
		xa_for_each(&shadow_ns_map, id, ns) {
			void *priv;

			if (ns->type != type || ns->type_priv)
				continue;
			priv = ops->priv_alloc(ns->parent_id, NULL);
			if (!IS_ERR(priv))
				ns->type_priv = priv;
		}
		mutex_unlock(&shadow_ns_map_lock);
	}

	pr_info("shadow_ns: registered type %u extension (real_support=%d)\n",
		type, ops->real_support);
	return 0;
}
EXPORT_SYMBOL_GPL(shadow_ns_base_register_type);

void shadow_ns_base_unregister_type(u32 type)
{
	const struct shadow_ns_type_ops *ops;
	struct shadow_ns *ns;
	unsigned long id;

	if (type >= SHADOW_NS_TYPE_MAX)
		return;

	mutex_lock(&shadow_ns_type_lock);
	ops = shadow_ns_type_ops_tbl[type];
	shadow_ns_type_ops_tbl[type] = NULL;
	mutex_unlock(&shadow_ns_type_lock);

	if (!ops)
		return;

	/*
	 * Reclaim every surviving payload of this type before the submodule's
	 * code (which owns priv_free) unloads. The namespaces themselves stay
	 * alive as bookkeeping-only objects with type_priv reset to NULL.
	 *
	 * This runs from the submodule's own module_exit(), so its priv_free
	 * code is still resident. Per-callback try_module_get() references taken
	 * elsewhere block that module_exit() until they are released, so no
	 * priv_alloc/priv_free callback can be executing concurrently here.
	 */
	if (ops->priv_free) {
		mutex_lock(&shadow_ns_map_lock);
		xa_for_each(&shadow_ns_map, id, ns) {
			if (ns->type != type || !ns->type_priv)
				continue;
			ops->priv_free(ns->type_priv);
			ns->type_priv = NULL;
		}
		mutex_unlock(&shadow_ns_map_lock);
	}

	pr_info("shadow_ns: unregistered type %u extension\n", type);
}
EXPORT_SYMBOL_GPL(shadow_ns_base_unregister_type);

bool shadow_ns_base_type_loaded(u32 type)
{
	return shadow_ns_type_ops_get(type) != NULL;
}
EXPORT_SYMBOL_GPL(shadow_ns_base_type_loaded);

bool shadow_ns_base_type_real(u32 type)
{
	const struct shadow_ns_type_ops *ops = shadow_ns_type_ops_get(type);

	return ops && ops->real_support;
}
EXPORT_SYMBOL_GPL(shadow_ns_base_type_real);

struct shadow_ns *shadow_ns_base_get_current(u32 type)
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
	return ns;
}
EXPORT_SYMBOL_GPL(shadow_ns_base_get_current);

void shadow_ns_base_put(struct shadow_ns *ns)
{
	shadow_ns_put(ns);
}
EXPORT_SYMBOL_GPL(shadow_ns_base_put);

void *shadow_ns_base_priv(struct shadow_ns *ns)
{
	return ns ? ns->type_priv : NULL;
}
EXPORT_SYMBOL_GPL(shadow_ns_base_priv);

static int __init shadow_ns_init(void)
{
	int ret;
	int hooked;

	pr_info("shadow_ns: init starting (base module version %s)\n",
		SHADOW_NS_VERSION);

	pr_info("shadow_ns: init: installing transparent syscall hooks\n");
	hooked = shadow_hook_install_all(shadow_ns_hooks, "shadow_ns");
	if (hooked < 0) {
		ret = hooked;
		pr_err("shadow_ns: init: shadow_hook_install_all() failed: %d\n", ret);
		return ret;
	}
	pr_info("shadow_ns: init: %d hook(s) installed\n", hooked);

	pr_info("shadow_ns: init: scheduling stale-task-group reap work\n");
	INIT_DELAYED_WORK(&shadow_ns_reap_work, shadow_ns_reap_workfn);
	schedule_delayed_work(&shadow_ns_reap_work, SHADOW_NS_REAP_INTERVAL);

	pr_info("shadow_ns: loaded (transparent hooks %d); per-type extensions register via shadow_ns_base_register_type()\n",
		hooked);
	return 0;
}

static void __exit shadow_ns_exit(void)
{
	struct shadow_ns *ns;
	struct shadow_task_group *tg;
	unsigned long id;

	/*
	 * Refuse any late shadow_ns_base_register_type() and stop handing out
	 * new type ops. A registered submodule holds a module reference on
	 * shadow_ns_base, so this exit path only runs once every per-type
	 * submodule has already unregistered.
	 */
	mutex_lock(&shadow_ns_type_lock);
	shadow_ns_base_exiting = true;
	mutex_unlock(&shadow_ns_type_lock);

	pr_info("shadow_ns: exit: removing transparent syscall hooks\n");
	shadow_hook_remove_all(shadow_ns_hooks);
	pr_info("shadow_ns: exit: cancelling reap work\n");
	cancel_delayed_work_sync(&shadow_ns_reap_work);

	for (;;) {
		id = 0;
		mutex_lock(&shadow_ns_tgid_lock);
		tg = xa_find(&shadow_ns_tgid_map, &id, ULONG_MAX, XA_PRESENT);
		if (tg)
			xa_erase(&shadow_ns_tgid_map, id);
		mutex_unlock(&shadow_ns_tgid_lock);
		if (!tg)
			break;
		shadow_ns_task_group_free(tg);
	}
	xa_destroy(&shadow_ns_tgid_map);

	mutex_lock(&shadow_ns_map_lock);
	xa_for_each(&shadow_ns_map, id, ns) {
		xa_erase(&shadow_ns_map, id);
		kfree(ns);
	}
	mutex_unlock(&shadow_ns_map_lock);
	xa_destroy(&shadow_ns_map);

	pr_info("shadow_ns: unloaded\n");
}

module_init(shadow_ns_init);
module_exit(shadow_ns_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Shadow namespace base: generic reference-counted namespace registry, transparent unshare/setns/clone/fork hooks, and a plugin API for per-type extension modules (shadow_ns_uts.ko, shadow_ns_net.ko, ...)");
MODULE_VERSION(SHADOW_NS_VERSION);
