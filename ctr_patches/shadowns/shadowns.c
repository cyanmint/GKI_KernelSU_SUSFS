// SPDX-License-Identifier: GPL-2.0
/*
 * shadowns - simulated ("shadow") namespace subsystem
 *
 * A standalone loadable kernel module that provides an independent,
 * reference-counted set of "shadow" namespace objects driven entirely from
 * userspace through ioctls on the /dev/shadowns misc device.
 *
 * Motivation
 * ----------
 * The native Linux namespace machinery (CONFIG_UTS_NS, CONFIG_IPC_NS,
 * CONFIG_PID_NS, CONFIG_NET_NS, CONFIG_USER_NS, ...) is compiled directly into
 * vmlinux: it adds fields to task_struct/nsproxy/cred and wires the
 * unshare(2)/setns(2)/clone(2) syscalls into the core kernel. None of that can
 * be added by a module after the kernel has been built, because the struct
 * layouts and syscall table are frozen at compile time.
 *
 * shadowns therefore does not try to hook those paths. Instead it maintains a
 * *parallel* namespace model that a patched container runtime opts into. Each
 * open file descriptor on /dev/shadowns is a "session" (think: one container
 * bring-up). A session can create, unshare, join (setns) and query shadow
 * namespaces of each type, exactly mirroring the native namespace verbs, and
 * the module tracks membership and reference counts on its own objects.
 *
 * What is actually simulated
 * --------------------------
 * - UTS: fully functional per-namespace nodename/domainname storage that a
 *   runtime can read back, so hostname isolation behaves as containers expect.
 * - IPC/MNT/PID/NET/USER/CGROUP: reference-counted membership bookkeeping with
 *   parent/child lineage. This gives a runtime stable namespace identities and
 *   join semantics, but does NOT provide real kernel-level isolation of those
 *   subsystems (that can only come from the native, compiled-in namespaces).
 *
 * Lifecycle is tied to the open fd: every namespace object is reference
 * counted, and all references held by a session are dropped when the fd is
 * closed, so nothing leaks even if the runtime crashes.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <linux/refcount.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/atomic.h>

#include "include/uapi/shadowns.h"

#define SHADOWNS_MAX_NS		65536

/*
 * struct shadow_ns - a single shadow namespace object.
 * @id:        stable identifier handed to userspace (xarray index)
 * @type:      enum shadowns_type
 * @parent_id: id of the namespace this was cloned from, 0 if none
 * @refcount:  dropped by every session that references this object
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

struct shadow_owned_ref {
	struct list_head	node;
	struct shadow_ns	*ns;
};

/* Global registry of shadow namespaces: id -> struct shadow_ns *. */
static DEFINE_XARRAY_ALLOC1(shadowns_map);
static DEFINE_MUTEX(shadowns_map_lock);
static atomic_t shadowns_count = ATOMIC_INIT(0);

static bool shadowns_type_valid(u32 type)
{
	return type < SHADOWNS_TYPE_MAX;
}

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

/* Join @ns as the session's current namespace of its type (consumes a ref). */
static void shadow_session_join(struct shadow_session *s, struct shadow_ns *ns)
{
	u32 type = ns->type;

	if (s->cur[type])
		shadowns_put(s->cur[type]);
	s->cur[type] = ns;
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
		shadow_session_join(s, ns);
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
	shadow_session_join(s, ns); /* consumes the ref from shadowns_get() */
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

static int __init shadowns_init(void)
{
	int ret;

	ret = misc_register(&shadowns_miscdev);
	if (ret) {
		pr_err("shadowns: failed to register misc device: %d\n", ret);
		return ret;
	}

	pr_info("shadowns: simulated namespace subsystem loaded (ABI v%d) at %s\n",
		SHADOWNS_ABI_VERSION, SHADOWNS_DEVICE_PATH);
	return 0;
}

static void __exit shadowns_exit(void)
{
	struct shadow_ns *ns;
	unsigned long id;

	misc_deregister(&shadowns_miscdev);

	/*
	 * All sessions are gone once the device is deregistered and no fds
	 * remain, but defensively free any objects that survived (e.g. leaked
	 * by a buggy client that never closed its fd before rmmod).
	 */
	mutex_lock(&shadowns_map_lock);
	xa_for_each(&shadowns_map, id, ns) {
		xa_erase(&shadowns_map, id);
		kfree(ns);
	}
	mutex_unlock(&shadowns_map_lock);
	xa_destroy(&shadowns_map);

	pr_info("shadowns: simulated namespace subsystem unloaded\n");
}

module_init(shadowns_init);
module_exit(shadowns_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Simulated (shadow) namespace subsystem for patched containerd");
MODULE_VERSION("1.0");
