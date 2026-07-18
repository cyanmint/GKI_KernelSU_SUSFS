// SPDX-License-Identifier: GPL-2.0
/*
 * shadowns - simulated ("shadow") namespace subsystem
 *
 * A standalone loadable kernel module that provides an independent,
 * reference-counted set of "shadow" namespace objects.
 *
 * The original implementation exposed those objects only through ioctls on the
 * /dev/shadowns misc device: each open fd was a private session that could
 * create/join/query shadow namespaces explicitly.
 *
 * The module now also installs ftrace-based hooks on the real syscall entry
 * points (unshare/setns/clone*/fork/vfork and UTS-related syscalls). That
 * second path keeps the original ioctl API intact for diagnostics and backward
 * compatibility, but also lets unmodified container runtimes drive the shadow
 * model by calling the stock syscalls.
 *
 * What is actually simulated
 * --------------------------
 * - UTS: fully functional per-namespace nodename/domainname storage.
 * - IPC/MNT/PID/NET/USER/CGROUP: reference-counted membership bookkeeping with
 *   parent/child lineage. These provide stable namespace identities and join
 *   semantics, but not real kernel-enforced isolation.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <linux/refcount.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/utsname.h>

#if IS_ENABLED(CONFIG_TRACEPOINTS)
#include <trace/events/sched.h>
#endif

#include "../shadow_hook/shadow_hook.h"
#include "include/uapi/shadowns.h"

#define SHADOWNS_MAX_NS		65536
#define SHADOWNS_SHADOW_CLONE_FLAGS	(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | \
				 CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUSER | \
				 CLONE_NEWCGROUP)

/*
 * struct shadow_ns - a single shadow namespace object.
 * @id:        stable identifier handed to userspace (xarray index)
 * @type:      enum shadowns_type
 * @parent_id: id of the namespace this was cloned from, 0 if none
 * @refcount:  dropped by every holder that references this object
 * @uts:       UTS payload, only meaningful when @type == SHADOWNS_TYPE_UTS
 */
struct shadow_ns {
	u32			id;
	u32			type;
	u32			parent_id;
	refcount_t		refcount;
	struct shadowns_uts	uts;
};

/*
 * struct shadow_session - per-open-fd state.
 * @cur:   current namespace of each type joined by this session (holds a ref)
 * @owned: namespaces created by this session that are not (or no longer) the
 *         current one, but whose creation reference is still held by the fd
 * @lock:  serialises operations within a session
 */
struct shadow_session {
	struct shadow_ns	*cur[SHADOWNS_TYPE_MAX];
	struct list_head	owned;
	struct mutex		lock;
};

/*
 * struct shadow_task_group - transparent syscall-facing state keyed by TGID.
 * @tgid: thread-group id (what userspace sees as PID for a process)
 * @cur:  current shadow namespace of each type for this task group
 * @lock: serialises updates to @cur and UTS payloads
 */
struct shadow_task_group {
	pid_t				 tgid;
	struct shadow_ns		*cur[SHADOWNS_TYPE_MAX];
	struct mutex			 lock;
};

struct shadow_owned_ref {
	struct list_head	node;
	struct shadow_ns	*ns;
};

static const struct file_operations shadowns_fops;

/* Global registry of shadow namespaces: id -> struct shadow_ns *. */
static DEFINE_XARRAY_ALLOC1(shadowns_map);
static DEFINE_MUTEX(shadowns_map_lock);
static atomic_t shadowns_count = ATOMIC_INIT(0);

/* Transparent task-group registry: tgid -> struct shadow_task_group *. */
static DEFINE_XARRAY(shadowns_tgid_map);
static DEFINE_MUTEX(shadowns_tgid_lock);

static long (*real_sys_unshare)(const struct pt_regs *regs);
static long (*real_sys_setns)(const struct pt_regs *regs);
static long (*real_sys_sethostname)(const struct pt_regs *regs);
static long (*real_sys_setdomainname)(const struct pt_regs *regs);
static long (*real_sys_newuname)(const struct pt_regs *regs);
static long (*real_sys_clone)(const struct pt_regs *regs);
static long (*real_sys_clone3)(const struct pt_regs *regs);
static long (*real_sys_fork)(const struct pt_regs *regs);
static long (*real_sys_vfork)(const struct pt_regs *regs);

static const char * const shadowns_unshare_names[] = {
	"__arm64_sys_unshare",
	"__x64_sys_unshare",
	"sys_unshare",
	NULL,
};
static const char * const shadowns_setns_names[] = {
	"__arm64_sys_setns",
	"__x64_sys_setns",
	"sys_setns",
	NULL,
};
static const char * const shadowns_sethostname_names[] = {
	"__arm64_sys_sethostname",
	"__x64_sys_sethostname",
	"sys_sethostname",
	NULL,
};
static const char * const shadowns_setdomainname_names[] = {
	"__arm64_sys_setdomainname",
	"__x64_sys_setdomainname",
	"sys_setdomainname",
	NULL,
};
static const char * const shadowns_newuname_names[] = {
	"__arm64_sys_newuname",
	"__x64_sys_newuname",
	"sys_newuname",
	"__arm64_sys_uname",
	"__x64_sys_uname",
	"sys_uname",
	NULL,
};
static const char * const shadowns_clone_names[] = {
	"__arm64_sys_clone",
	"__x64_sys_clone",
	"sys_clone",
	NULL,
};
static const char * const shadowns_clone3_names[] = {
	"__arm64_sys_clone3",
	"__x64_sys_clone3",
	"sys_clone3",
	NULL,
};
static const char * const shadowns_fork_names[] = {
	"__arm64_sys_fork",
	"__x64_sys_fork",
	"sys_fork",
	NULL,
};
static const char * const shadowns_vfork_names[] = {
	"__arm64_sys_vfork",
	"__x64_sys_vfork",
	"sys_vfork",
	NULL,
};

static long shadowns_hook_unshare(const struct pt_regs *regs);
static long shadowns_hook_setns(const struct pt_regs *regs);
static long shadowns_hook_sethostname(const struct pt_regs *regs);
static long shadowns_hook_setdomainname(const struct pt_regs *regs);
static long shadowns_hook_newuname(const struct pt_regs *regs);
static long shadowns_hook_clone(const struct pt_regs *regs);
static long shadowns_hook_clone3(const struct pt_regs *regs);
static long shadowns_hook_fork(const struct pt_regs *regs);
static long shadowns_hook_vfork(const struct pt_regs *regs);

static struct shadow_hook shadowns_unshare_hook =
	SHADOW_HOOK(shadowns_unshare_names, shadowns_hook_unshare,
		    &real_sys_unshare);
static struct shadow_hook shadowns_setns_hook =
	SHADOW_HOOK(shadowns_setns_names, shadowns_hook_setns, &real_sys_setns);
static struct shadow_hook shadowns_sethostname_hook =
	SHADOW_HOOK(shadowns_sethostname_names, shadowns_hook_sethostname,
		    &real_sys_sethostname);
static struct shadow_hook shadowns_setdomainname_hook =
	SHADOW_HOOK(shadowns_setdomainname_names, shadowns_hook_setdomainname,
		    &real_sys_setdomainname);
static struct shadow_hook shadowns_newuname_hook =
	SHADOW_HOOK(shadowns_newuname_names, shadowns_hook_newuname,
		    &real_sys_newuname);
static struct shadow_hook shadowns_clone_hook =
	SHADOW_HOOK(shadowns_clone_names, shadowns_hook_clone, &real_sys_clone);
static struct shadow_hook shadowns_clone3_hook =
	SHADOW_HOOK(shadowns_clone3_names, shadowns_hook_clone3, &real_sys_clone3);
static struct shadow_hook shadowns_fork_hook =
	SHADOW_HOOK(shadowns_fork_names, shadowns_hook_fork, &real_sys_fork);
static struct shadow_hook shadowns_vfork_hook =
	SHADOW_HOOK(shadowns_vfork_names, shadowns_hook_vfork, &real_sys_vfork);

static struct shadow_hook *shadowns_hooks[] = {
	&shadowns_unshare_hook,
	&shadowns_setns_hook,
	&shadowns_sethostname_hook,
	&shadowns_setdomainname_hook,
	&shadowns_newuname_hook,
	&shadowns_clone_hook,
	&shadowns_clone3_hook,
	&shadowns_fork_hook,
	&shadowns_vfork_hook,
	NULL,
};

static bool shadowns_type_valid(u32 type)
{
	return type < SHADOWNS_TYPE_MAX;
}

static unsigned long shadowns_type_to_clone_flag(u32 type)
{
	switch (type) {
	case SHADOWNS_TYPE_UTS:
		return CLONE_NEWUTS;
	case SHADOWNS_TYPE_IPC:
		return CLONE_NEWIPC;
	case SHADOWNS_TYPE_MNT:
		return CLONE_NEWNS;
	case SHADOWNS_TYPE_PID:
		return CLONE_NEWPID;
	case SHADOWNS_TYPE_NET:
		return CLONE_NEWNET;
	case SHADOWNS_TYPE_USER:
		return CLONE_NEWUSER;
	case SHADOWNS_TYPE_CGROUP:
		return CLONE_NEWCGROUP;
	default:
		return 0;
	}
}

static int shadowns_clone_flag_to_type(unsigned long flag)
{
	switch (flag) {
	case 0:
		return SHADOWNS_TYPE_MAX;
	case CLONE_NEWUTS:
		return SHADOWNS_TYPE_UTS;
	case CLONE_NEWIPC:
		return SHADOWNS_TYPE_IPC;
	case CLONE_NEWNS:
		return SHADOWNS_TYPE_MNT;
	case CLONE_NEWPID:
		return SHADOWNS_TYPE_PID;
	case CLONE_NEWNET:
		return SHADOWNS_TYPE_NET;
	case CLONE_NEWUSER:
		return SHADOWNS_TYPE_USER;
	case CLONE_NEWCGROUP:
		return SHADOWNS_TYPE_CGROUP;
	default:
		return -EINVAL;
	}
}

static bool shadowns_requires_admin(unsigned long shadow_flags)
{
	return shadow_flags != 0;
}

#if defined(CONFIG_ARM64)
static unsigned long shadowns_sys_arg0(const struct pt_regs *regs)
{
	return regs->regs[0];
}

static unsigned long shadowns_sys_arg1(const struct pt_regs *regs)
{
	return regs->regs[1];
}

static void shadowns_sys_set_arg0(struct pt_regs *regs, unsigned long value)
{
	regs->regs[0] = value;
}
#elif defined(CONFIG_X86_64)
static unsigned long shadowns_sys_arg0(const struct pt_regs *regs)
{
	return regs->di;
}

static unsigned long shadowns_sys_arg1(const struct pt_regs *regs)
{
	return regs->si;
}

static void shadowns_sys_set_arg0(struct pt_regs *regs, unsigned long value)
{
	regs->di = value;
}
#else
#error "shadowns: unsupported architecture"
#endif

/* Allocate a new shadow namespace with refcount 1. Caller owns the reference. */
static struct shadow_ns *shadowns_alloc(u32 type, u32 parent_id,
					const struct shadowns_uts *inherit)
{
	struct shadow_ns *ns;
	u32 id;
	int ret;

	if (atomic_read(&shadowns_count) >= SHADOWNS_MAX_NS)
		return ERR_PTR(-ENOSPC);

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return ERR_PTR(-ENOMEM);

	ns->type = type;
	ns->parent_id = parent_id;
	refcount_set(&ns->refcount, 1);
	if (type == SHADOWNS_TYPE_UTS && inherit)
		ns->uts = *inherit;

	mutex_lock(&shadowns_map_lock);
	/* Reserve id in [1, INT_MAX]; 0 is reserved to mean "no namespace". */
	ret = xa_alloc(&shadowns_map, &id, ns, XA_LIMIT(1, INT_MAX), GFP_KERNEL);
	mutex_unlock(&shadowns_map_lock);
	if (ret) {
		kfree(ns);
		return ERR_PTR(ret);
	}

	ns->id = id;
	atomic_inc(&shadowns_count);
	return ns;
}

static struct shadow_ns *shadowns_alloc_derived(u32 type, struct shadow_ns *parent)
{
	const struct shadowns_uts *inherit = NULL;
	u32 parent_id = 0;

	if (parent) {
		parent_id = parent->id;
		if (type == SHADOWNS_TYPE_UTS)
			inherit = &parent->uts;
	}

	return shadowns_alloc(type, parent_id, inherit);
}

static struct shadow_ns *shadowns_grab(struct shadow_ns *ns)
{
	if (!ns)
		return NULL;
	if (!refcount_inc_not_zero(&ns->refcount))
		return NULL;
	return ns;
}

static struct shadow_ns *shadowns_get(u32 id)
{
	struct shadow_ns *ns;

	if (!id)
		return NULL;

	mutex_lock(&shadowns_map_lock);
	ns = xa_load(&shadowns_map, id);
	if (ns && !refcount_inc_not_zero(&ns->refcount))
		ns = NULL;
	mutex_unlock(&shadowns_map_lock);
	return ns;
}

static void shadowns_put(struct shadow_ns *ns)
{
	if (!ns)
		return;

	if (refcount_dec_and_test(&ns->refcount)) {
		mutex_lock(&shadowns_map_lock);
		xa_erase(&shadowns_map, ns->id);
		mutex_unlock(&shadowns_map_lock);
		atomic_dec(&shadowns_count);
		kfree(ns);
	}
}

static void shadowns_drop_cur_array(struct shadow_ns **cur)
{
	int type;

	for (type = 0; type < SHADOWNS_TYPE_MAX; type++) {
		shadowns_put(cur[type]);
		cur[type] = NULL;
	}
}

static void shadow_ns_slot_replace(struct shadow_ns **slot, struct shadow_ns *ns)
{
	if (*slot)
		shadowns_put(*slot);
	*slot = ns;
}

static struct shadow_task_group *shadowns_task_group_lookup(pid_t tgid)
{
	struct shadow_task_group *tg;

	if (tgid <= 0)
		return NULL;

	mutex_lock(&shadowns_tgid_lock);
	tg = xa_load(&shadowns_tgid_map, tgid);
	mutex_unlock(&shadowns_tgid_lock);
	return tg;
}

static struct shadow_task_group *shadowns_task_group_get_or_create(pid_t tgid)
{
	struct shadow_task_group *tg;
	int ret;

	if (tgid <= 0)
		return ERR_PTR(-ESRCH);

	mutex_lock(&shadowns_tgid_lock);
	tg = xa_load(&shadowns_tgid_map, tgid);
	if (tg)
		goto out_unlock;

	tg = kzalloc(sizeof(*tg), GFP_KERNEL);
	if (!tg) {
		mutex_unlock(&shadowns_tgid_lock);
		return ERR_PTR(-ENOMEM);
	}

	tg->tgid = tgid;
	mutex_init(&tg->lock);
	ret = xa_err(xa_store(&shadowns_tgid_map, tgid, tg, GFP_KERNEL));
	if (ret) {
		mutex_destroy(&tg->lock);
		kfree(tg);
		mutex_unlock(&shadowns_tgid_lock);
		return ERR_PTR(ret);
	}

out_unlock:
	mutex_unlock(&shadowns_tgid_lock);
	return tg;
}

static struct shadow_task_group *shadowns_current_task_group(bool create)
{
	pid_t tgid = task_tgid_nr(current);

	if (create)
		return shadowns_task_group_get_or_create(tgid);
	return shadowns_task_group_lookup(tgid);
}

static void shadowns_task_group_free(struct shadow_task_group *tg)
{
	if (!tg)
		return;

	shadowns_drop_cur_array(tg->cur);
	mutex_destroy(&tg->lock);
	kfree(tg);
}

static void shadowns_task_group_drop(pid_t tgid)
{
	struct shadow_task_group *tg;

	if (tgid <= 0)
		return;

	mutex_lock(&shadowns_tgid_lock);
	tg = xa_erase(&shadowns_tgid_map, tgid);
	mutex_unlock(&shadowns_tgid_lock);
	shadowns_task_group_free(tg);
}

static int shadowns_task_group_unshare_locked(struct shadow_task_group *tg,
					      unsigned long shadow_flags)
{
	struct shadow_ns *created[SHADOWNS_TYPE_MAX] = { };
	int type;
	int ret = 0;

	for (type = 0; type < SHADOWNS_TYPE_MAX; type++) {
		if (!(shadow_flags & shadowns_type_to_clone_flag(type)))
			continue;

		created[type] = shadowns_alloc_derived(type, tg->cur[type]);
		if (IS_ERR(created[type])) {
			ret = PTR_ERR(created[type]);
			created[type] = NULL;
			goto err_put;
		}
	}

	for (type = 0; type < SHADOWNS_TYPE_MAX; type++) {
		if (created[type])
			shadow_ns_slot_replace(&tg->cur[type], created[type]);
	}

	return 0;

err_put:
	for (type = 0; type < SHADOWNS_TYPE_MAX; type++)
		shadowns_put(created[type]);
	return ret;
}

static int shadowns_prepare_child_cur_locked(struct shadow_task_group *parent,
					     unsigned long shadow_flags,
					     struct shadow_ns **next)
{
	int type;
	int ret;

	for (type = 0; type < SHADOWNS_TYPE_MAX; type++) {
		struct shadow_ns *parent_ns = parent ? parent->cur[type] : NULL;

		if (shadow_flags & shadowns_type_to_clone_flag(type)) {
			next[type] = shadowns_alloc_derived(type, parent_ns);
			if (IS_ERR(next[type])) {
				ret = PTR_ERR(next[type]);
				next[type] = NULL;
				goto err_put;
			}
			continue;
		}

		next[type] = shadowns_grab(parent_ns);
	}

	return 0;

err_put:
	for (type = 0; type < SHADOWNS_TYPE_MAX; type++) {
		shadowns_put(next[type]);
		next[type] = NULL;
	}
	return ret;
}

static int shadowns_install_child_state(pid_t child_tgid,
					struct shadow_task_group *parent,
					unsigned long shadow_flags)
{
	struct shadow_task_group *child;
	struct shadow_ns *next[SHADOWNS_TYPE_MAX] = { };
	int ret;

	if (child_tgid <= 0)
		return -ESRCH;

	child = shadowns_task_group_get_or_create(child_tgid);
	if (IS_ERR(child))
		return PTR_ERR(child);

	if (parent)
		mutex_lock(&parent->lock);
	ret = shadowns_prepare_child_cur_locked(parent, shadow_flags, next);
	if (parent)
		mutex_unlock(&parent->lock);
	if (ret)
		return ret;

	mutex_lock(&child->lock);
	shadowns_drop_cur_array(child->cur);
	memcpy(child->cur, next, sizeof(child->cur));
	memset(next, 0, sizeof(next));
	mutex_unlock(&child->lock);
	return 0;
}

/* Record an extra creation reference owned by this session's fd. */
static int shadow_session_own(struct shadow_session *s, struct shadow_ns *ns)
{
	struct shadow_owned_ref *ref;

	ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (!ref)
		return -ENOMEM;

	ref->ns = ns;
	list_add(&ref->node, &s->owned);
	return 0;
}

/* Join @ns as the current namespace of its type (consumes a ref). */
static void shadow_join_cur(struct shadow_ns **cur, struct shadow_ns *ns)
{
	shadow_ns_slot_replace(&cur[ns->type], ns);
}

static long shadowns_ioc_create(struct shadow_session *s, void __user *arg,
				bool join)
{
	struct shadowns_create req;
	struct shadow_ns *ns;
	struct shadowns_uts *inherit = NULL;
	u32 parent_id = 0;
	int ret;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (!shadowns_type_valid(req.type) || req.flags != 0)
		return -EINVAL;

	mutex_lock(&s->lock);

	/* Derive from the session's current namespace of this type, if any. */
	if (s->cur[req.type]) {
		parent_id = s->cur[req.type]->id;
		if (req.type == SHADOWNS_TYPE_UTS)
			inherit = &s->cur[req.type]->uts;
	}

	ns = shadowns_alloc(req.type, parent_id, inherit);
	if (IS_ERR(ns)) {
		mutex_unlock(&s->lock);
		return PTR_ERR(ns);
	}

	if (join) {
		/* UNSHARE: the current-membership slot takes the creation ref. */
		shadow_join_cur(s->cur, ns);
	} else {
		/* CREATE: the fd retains the creation ref until close/destroy. */
		ret = shadow_session_own(s, ns);
		if (ret) {
			shadowns_put(ns);
			mutex_unlock(&s->lock);
			return ret;
		}
	}

	req.id = ns->id;
	req.parent_id = ns->parent_id;
	mutex_unlock(&s->lock);

	if (copy_to_user(arg, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static long shadowns_ioc_setns(struct shadow_session *s, void __user *arg)
{
	struct shadowns_setns req;
	struct shadow_ns *ns;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	ns = shadowns_get(req.id);
	if (!ns)
		return -ENOENT;

	/* Optionally validate the caller's expectation of the namespace type. */
	if (req.type != SHADOWNS_TYPE_MAX && req.type != ns->type) {
		shadowns_put(ns);
		return -EINVAL;
	}

	mutex_lock(&s->lock);
	shadow_join_cur(s->cur, ns); /* consumes the ref from shadowns_get() */
	mutex_unlock(&s->lock);
	return 0;
}

static long shadowns_ioc_get(struct shadow_session *s, void __user *arg)
{
	struct shadowns_get req;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (!shadowns_type_valid(req.type))
		return -EINVAL;

	mutex_lock(&s->lock);
	req.id = s->cur[req.type] ? s->cur[req.type]->id : 0;
	mutex_unlock(&s->lock);

	if (copy_to_user(arg, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static long shadowns_ioc_destroy(struct shadow_session *s, void __user *arg)
{
	struct shadow_owned_ref *ref, *tmp;
	u32 id;
	int type;
	bool found = false;

	if (copy_from_user(&id, arg, sizeof(id)))
		return -EFAULT;
	if (!id)
		return -EINVAL;

	mutex_lock(&s->lock);

	/* Drop a matching owned (created-but-not-joined) reference. */
	list_for_each_entry_safe(ref, tmp, &s->owned, node) {
		if (ref->ns->id == id) {
			list_del(&ref->node);
			shadowns_put(ref->ns);
			kfree(ref);
			found = true;
			break;
		}
	}

	/* Also release it as a current membership if the session joined it. */
	for (type = 0; type < SHADOWNS_TYPE_MAX; type++) {
		if (s->cur[type] && s->cur[type]->id == id) {
			shadowns_put(s->cur[type]);
			s->cur[type] = NULL;
			found = true;
		}
	}

	mutex_unlock(&s->lock);
	return found ? 0 : -ENOENT;
}

static long shadowns_ioc_set_uts(struct shadow_session *s, void __user *arg)
{
	struct shadowns_uts req;
	struct shadow_ns *ns;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	/* Guarantee NUL termination regardless of what userspace supplied. */
	req.nodename[SHADOWNS_UTS_LEN] = '\0';
	req.domainname[SHADOWNS_UTS_LEN] = '\0';

	mutex_lock(&s->lock);
	ns = s->cur[SHADOWNS_TYPE_UTS];
	if (!ns) {
		mutex_unlock(&s->lock);
		return -ENOENT;
	}
	ns->uts = req;
	mutex_unlock(&s->lock);
	return 0;
}

static long shadowns_ioc_get_uts(struct shadow_session *s, void __user *arg)
{
	struct shadowns_uts req;
	struct shadow_ns *ns;

	mutex_lock(&s->lock);
	ns = s->cur[SHADOWNS_TYPE_UTS];
	if (!ns) {
		mutex_unlock(&s->lock);
		return -ENOENT;
	}
	req = ns->uts;
	mutex_unlock(&s->lock);

	if (copy_to_user(arg, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static int shadowns_join_task_group_ns(struct shadow_task_group *tg,
				       struct shadow_ns *ns)
{
	if (!tg || !ns)
		return -EINVAL;

	mutex_lock(&tg->lock);
	shadow_join_cur(tg->cur, ns);
	mutex_unlock(&tg->lock);
	return 0;
}

static long shadowns_task_group_setns_by_id(int id, int flags)
{
	struct shadow_task_group *tg;
	struct shadow_ns *ns;
	int wanted_type;
	long ret;

	wanted_type = shadowns_clone_flag_to_type(flags);
	if (wanted_type < 0)
		return wanted_type;

	ns = shadowns_get(id);
	if (!ns)
		return -ENOENT;
	if (wanted_type != SHADOWNS_TYPE_MAX && ns->type != wanted_type) {
		shadowns_put(ns);
		return -EINVAL;
	}
	if (!capable(CAP_SYS_ADMIN)) {
		shadowns_put(ns);
		return -EPERM;
	}

	tg = shadowns_current_task_group(true);
	if (IS_ERR(tg)) {
		shadowns_put(ns);
		return PTR_ERR(tg);
	}

	ret = shadowns_join_task_group_ns(tg, ns);
	return ret;
}

static long shadowns_task_group_setns_by_session_fd(int fd, int flags)
{
	struct file *file;
	struct shadow_session *s;
	struct shadow_task_group *tg;
	struct shadow_ns *ns = NULL;
	int type;

	type = shadowns_clone_flag_to_type(flags);
	if (type < 0)
		return type;

	file = fget(fd);
	if (!file)
		return -EBADF;
	if (file->f_op != &shadowns_fops) {
		fput(file);
		return -EINVAL;
	}

	s = file->private_data;
	if (!s) {
		fput(file);
		return -ENOENT;
	}

	mutex_lock(&s->lock);
	if (type == SHADOWNS_TYPE_MAX) {
		for (type = 0; type < SHADOWNS_TYPE_MAX; type++) {
			ns = shadowns_grab(s->cur[type]);
			if (ns)
				break;
		}
	} else {
		ns = shadowns_grab(s->cur[type]);
	}
	mutex_unlock(&s->lock);
	fput(file);

	if (!ns)
		return -ENOENT;
	if (!capable(CAP_SYS_ADMIN)) {
		shadowns_put(ns);
		return -EPERM;
	}

	tg = shadowns_current_task_group(true);
	if (IS_ERR(tg)) {
		shadowns_put(ns);
		return PTR_ERR(tg);
	}

	return shadowns_join_task_group_ns(tg, ns);
}

static void shadowns_capture_shadow_uts(struct shadowns_uts *uts, bool *has_uts)
{
	struct shadow_task_group *tg = shadowns_current_task_group(false);

	*has_uts = false;
	if (!tg)
		return;

	mutex_lock(&tg->lock);
	if (tg->cur[SHADOWNS_TYPE_UTS]) {
		*uts = tg->cur[SHADOWNS_TYPE_UTS]->uts;
		*has_uts = true;
	}
	mutex_unlock(&tg->lock);
}

static long shadowns_update_shadow_uts(bool domainname,
				       const char __user *name, int len)
{
	struct shadow_task_group *tg = shadowns_current_task_group(false);
	struct shadow_ns *ns;
	char buf[SHADOWNS_UTS_LEN + 1] = { 0 };

	if (!tg)
		return -ENOENT;
	if (len < 0 || len > SHADOWNS_UTS_LEN)
		return -EINVAL;

	mutex_lock(&tg->lock);
	ns = tg->cur[SHADOWNS_TYPE_UTS];
	if (!ns) {
		mutex_unlock(&tg->lock);
		return -ENOENT;
	}
	if (!capable(CAP_SYS_ADMIN)) {
		mutex_unlock(&tg->lock);
		return -EPERM;
	}
	if (len && copy_from_user(buf, name, len)) {
		mutex_unlock(&tg->lock);
		return -EFAULT;
	}
	buf[len] = '\0';
	if (domainname)
		strscpy(ns->uts.domainname, buf, sizeof(ns->uts.domainname));
	else
		strscpy(ns->uts.nodename, buf, sizeof(ns->uts.nodename));
	mutex_unlock(&tg->lock);
	return 0;
}

static pid_t shadowns_resolve_child_tgid(pid_t pid)
{
	struct task_struct *task;
	pid_t tgid = 0;

	if (pid <= 0)
		return 0;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return 0;

	tgid = task_tgid_nr(task);
	put_task_struct(task);
	return tgid;
}

static long shadowns_clone_finalize(long ret, struct shadow_task_group *parent,
				    unsigned long shadow_flags)
{
	pid_t child_tgid;
	int err;

	if (ret <= 0)
		return ret;
	if (!parent && !shadow_flags)
		return ret;

	child_tgid = shadowns_resolve_child_tgid((pid_t)ret);
	if (!child_tgid || child_tgid == task_tgid_nr(current))
		return ret;

	err = shadowns_install_child_state(child_tgid, parent, shadow_flags);
	if (err)
		pr_warn("shadowns: failed to install child state for tgid %d: %d\n",
			child_tgid, err);
	return ret;
}

static long shadowns_hook_unshare(const struct pt_regs *regs)
{
	struct shadow_task_group *tg;
	struct pt_regs regs_copy;
	unsigned long flags = shadowns_sys_arg0(regs);
	unsigned long shadow_flags = flags & SHADOWNS_SHADOW_CLONE_FLAGS;
	unsigned long native_flags = flags & ~SHADOWNS_SHADOW_CLONE_FLAGS;
	long ret = 0;

	if (!shadow_flags)
		return real_sys_unshare(regs);
	if (shadowns_requires_admin(shadow_flags) && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (native_flags || !shadow_flags) {
		regs_copy = *regs;
		shadowns_sys_set_arg0(&regs_copy, native_flags);
		ret = real_sys_unshare(&regs_copy);
		if (ret)
			return ret;
	}

	tg = shadowns_current_task_group(true);
	if (IS_ERR(tg))
		return PTR_ERR(tg);

	mutex_lock(&tg->lock);
	ret = shadowns_task_group_unshare_locked(tg, shadow_flags);
	mutex_unlock(&tg->lock);
	return ret;
}

static long shadowns_hook_setns(const struct pt_regs *regs)
{
	int fd = (int)shadowns_sys_arg0(regs);
	int flags = (int)shadowns_sys_arg1(regs);
	long ret = real_sys_setns(regs);
	long shadow_ret;

	if (ret != -EINVAL && ret != -ENOTTY)
		return ret;

	shadow_ret = shadowns_task_group_setns_by_session_fd(fd, flags);
	if (!shadow_ret)
		return 0;
	if (shadow_ret != -EBADF)
		return ret;

	/*
	 * Best-effort private encoding for shadow-only joins: use the numeric
	 * namespace id directly in place of @fd when there is no real nsfs fd.
	 */
	shadow_ret = shadowns_task_group_setns_by_id(fd, flags);
	if (!shadow_ret)
		return 0;

	return ret;
}

static long shadowns_hook_sethostname(const struct pt_regs *regs)
{
	const char __user *name = (const char __user *)(uintptr_t)shadowns_sys_arg0(regs);
	int len = (int)shadowns_sys_arg1(regs);
	long ret = shadowns_update_shadow_uts(false, name, len);

	if (ret == -ENOENT)
		return real_sys_sethostname(regs);
	return ret;
}

static long shadowns_hook_setdomainname(const struct pt_regs *regs)
{
	const char __user *name = (const char __user *)(uintptr_t)shadowns_sys_arg0(regs);
	int len = (int)shadowns_sys_arg1(regs);
	long ret = shadowns_update_shadow_uts(true, name, len);

	if (ret == -ENOENT)
		return real_sys_setdomainname(regs);
	return ret;
}

static long shadowns_hook_newuname(const struct pt_regs *regs)
{
	struct new_utsname uts;
	struct shadowns_uts shadow_uts;
	void __user *uarg = (void __user *)(uintptr_t)shadowns_sys_arg0(regs);
	bool has_shadow_uts;
	long ret;

	shadowns_capture_shadow_uts(&shadow_uts, &has_shadow_uts);
	ret = real_sys_newuname(regs);
	if (ret || !has_shadow_uts)
		return ret;

	if (copy_from_user(&uts, uarg, sizeof(uts)))
		return -EFAULT;
	strscpy(uts.nodename, shadow_uts.nodename, sizeof(uts.nodename));
	strscpy(uts.domainname, shadow_uts.domainname, sizeof(uts.domainname));
	if (copy_to_user(uarg, &uts, sizeof(uts)))
		return -EFAULT;
	return 0;
}

static long shadowns_hook_clone(const struct pt_regs *regs)
{
	struct shadow_task_group *parent = shadowns_current_task_group(false);
	struct pt_regs regs_copy = *regs;
	unsigned long flags = shadowns_sys_arg0(regs);
	unsigned long shadow_flags = flags & SHADOWNS_SHADOW_CLONE_FLAGS;
	long ret;

	if (shadow_flags && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (shadow_flags)
		shadowns_sys_set_arg0(&regs_copy, flags & ~SHADOWNS_SHADOW_CLONE_FLAGS);
	ret = real_sys_clone(&regs_copy);
	return shadowns_clone_finalize(ret, parent, shadow_flags);
}

static long shadowns_hook_clone3(const struct pt_regs *regs)
{
	struct shadow_task_group *parent = shadowns_current_task_group(false);
	void __user *uargs = (void __user *)(uintptr_t)shadowns_sys_arg0(regs);
	u64 orig_flags;
	u64 native_flags;
	unsigned long shadow_flags;
	long ret;
	bool patched = false;

	if (!uargs || copy_from_user(&orig_flags, uargs, sizeof(orig_flags)))
		return real_sys_clone3(regs);

	shadow_flags = (unsigned long)(orig_flags & SHADOWNS_SHADOW_CLONE_FLAGS);
	if (shadow_flags && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	native_flags = orig_flags & ~((u64)SHADOWNS_SHADOW_CLONE_FLAGS);
	if (shadow_flags) {
		if (copy_to_user(uargs, &native_flags, sizeof(native_flags)))
			return -EFAULT;
		patched = true;
	}

	ret = real_sys_clone3(regs);

	if (patched && copy_to_user(uargs, &orig_flags, sizeof(orig_flags)))
		pr_warn("shadowns: failed to restore clone3 flags for current task\n");

	return shadowns_clone_finalize(ret, parent, shadow_flags);
}

static long shadowns_hook_fork(const struct pt_regs *regs)
{
	struct shadow_task_group *parent = shadowns_current_task_group(false);
	long ret = real_sys_fork(regs);

	return shadowns_clone_finalize(ret, parent, 0);
}

static long shadowns_hook_vfork(const struct pt_regs *regs)
{
	struct shadow_task_group *parent = shadowns_current_task_group(false);
	long ret = real_sys_vfork(regs);

	return shadowns_clone_finalize(ret, parent, 0);
}

static long shadowns_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct shadow_session *s = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case SHADOWNS_IOC_ABI_VERSION: {
		u32 ver = SHADOWNS_ABI_VERSION;

		if (copy_to_user(uarg, &ver, sizeof(ver)))
			return -EFAULT;
		return 0;
	}
	case SHADOWNS_IOC_CREATE:
		return shadowns_ioc_create(s, uarg, false);
	case SHADOWNS_IOC_UNSHARE:
		return shadowns_ioc_create(s, uarg, true);
	case SHADOWNS_IOC_SETNS:
		return shadowns_ioc_setns(s, uarg);
	case SHADOWNS_IOC_GET:
		return shadowns_ioc_get(s, uarg);
	case SHADOWNS_IOC_DESTROY:
		return shadowns_ioc_destroy(s, uarg);
	case SHADOWNS_IOC_SET_UTS:
		return shadowns_ioc_set_uts(s, uarg);
	case SHADOWNS_IOC_GET_UTS:
		return shadowns_ioc_get_uts(s, uarg);
	default:
		return -ENOTTY;
	}
}

static int shadowns_open(struct inode *inode, struct file *file)
{
	struct shadow_session *s;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	INIT_LIST_HEAD(&s->owned);
	mutex_init(&s->lock);
	file->private_data = s;
	return 0;
}

static int shadowns_release(struct inode *inode, struct file *file)
{
	struct shadow_session *s = file->private_data;
	struct shadow_owned_ref *ref, *tmp;
	int type;

	if (!s)
		return 0;

	/* Drop every reference this session still holds; nothing may leak. */
	list_for_each_entry_safe(ref, tmp, &s->owned, node) {
		list_del(&ref->node);
		shadowns_put(ref->ns);
		kfree(ref);
	}
	for (type = 0; type < SHADOWNS_TYPE_MAX; type++)
		shadowns_put(s->cur[type]);

	mutex_destroy(&s->lock);
	kfree(s);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations shadowns_fops = {
	.owner		= THIS_MODULE,
	.open		= shadowns_open,
	.release	= shadowns_release,
	.unlocked_ioctl	= shadowns_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= no_llseek,
};

static struct miscdevice shadowns_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= SHADOWNS_DEVICE_NAME,
	.fops	= &shadowns_fops,
	.mode	= 0600,
};

#if IS_ENABLED(CONFIG_TRACEPOINTS)
static void shadowns_sched_process_exit(void *ignore, struct task_struct *p)
{
	if (!p->signal)
		return;
	if (atomic_read(&p->signal->live) != 0)
		return;

	shadowns_task_group_drop(task_tgid_nr(p));
}
#endif

static int __init shadowns_init(void)
{
	int ret;
	int hooked;

	ret = misc_register(&shadowns_miscdev);
	if (ret) {
		pr_err("shadowns: failed to register misc device: %d\n", ret);
		return ret;
	}

#if IS_ENABLED(CONFIG_TRACEPOINTS)
	ret = register_trace_sched_process_exit(shadowns_sched_process_exit, NULL);
	if (ret) {
		pr_err("shadowns: failed to register sched_process_exit tracepoint: %d\n",
		       ret);
		goto err_misc;
	}
#else
	ret = -EOPNOTSUPP;
	pr_err("shadowns: transparent task-group cleanup requires CONFIG_TRACEPOINTS\n");
	goto err_misc;
#endif

	hooked = shadow_hook_install_all(shadowns_hooks, "shadowns");
	if (hooked < 0) {
		ret = hooked;
		goto err_trace;
	}

	pr_info("shadowns: loaded (ABI v%d, device %s, transparent hooks %d)\n",
		SHADOWNS_ABI_VERSION, SHADOWNS_DEVICE_PATH, hooked);
	return 0;

err_trace:
#if IS_ENABLED(CONFIG_TRACEPOINTS)
	unregister_trace_sched_process_exit(shadowns_sched_process_exit, NULL);
#endif
err_misc:
	misc_deregister(&shadowns_miscdev);
	return ret;
}

static void __exit shadowns_exit(void)
{
	struct shadow_ns *ns;
	struct shadow_task_group *tg;
	unsigned long id;

	shadow_hook_remove_all(shadowns_hooks);
#if IS_ENABLED(CONFIG_TRACEPOINTS)
	unregister_trace_sched_process_exit(shadowns_sched_process_exit, NULL);
#endif
	misc_deregister(&shadowns_miscdev);

	for (;;) {
		id = 0;
		mutex_lock(&shadowns_tgid_lock);
		tg = xa_find(&shadowns_tgid_map, &id, ULONG_MAX, XA_PRESENT);
		if (tg)
			xa_erase(&shadowns_tgid_map, id);
		mutex_unlock(&shadowns_tgid_lock);
		if (!tg)
			break;
		shadowns_task_group_free(tg);
	}
	xa_destroy(&shadowns_tgid_map);

	/*
	 * All sessions are gone once the device is deregistered and no fds remain,
	 * but defensively free any objects that survived.
	 */
	mutex_lock(&shadowns_map_lock);
	xa_for_each(&shadowns_map, id, ns) {
		xa_erase(&shadowns_map, id);
		kfree(ns);
	}
	mutex_unlock(&shadowns_map_lock);
	xa_destroy(&shadowns_map);

	pr_info("shadowns: unloaded\n");
}

module_init(shadowns_init);
module_exit(shadowns_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Simulated (shadow) namespace subsystem with transparent syscall hooks");
MODULE_VERSION("1.1");
