// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_sysvipc - simulated System V IPC subsystem
 *
 * A standalone loadable kernel module that provides bookkeeping for virtual
 * SysV IPC resources (message queues, semaphore sets, shared-memory segments)
 * through ioctls on the /dev/shadow_sysvipc misc device.
 *
 * Motivation
 * ----------
 * The native SysV IPC machinery (CONFIG_SYSVIPC) is compiled into vmlinux.
 * It installs the msgget/msgsnd/msgrcv/semget/semop/shmget/shmat family of
 * syscalls and the /proc/sysvipc accounting.  None of that can be added by a
 * module after the kernel is built.
 *
 * shadow_sysvipc does not try to hook those paths.  Instead it maintains a
 * parallel set of virtual IPC objects—message queues, semaphore sets, and
 * shared-memory segments—that a patched container runtime can create, query,
 * and destroy through ioctls, so that IPC resource bookkeeping works even on
 * kernels built without CONFIG_SYSVIPC.
 *
 * What is simulated
 * -----------------
 * - Virtual resource objects with stable integer ids.
 * - Key-based lookup semantics mirroring msgget(2)/semget(2)/shmget(2).
 * - Reference counting tied to open file descriptors (sessions): resources
 *   created by a session are automatically freed when the fd is closed.
 *
 * What is NOT simulated
 * ---------------------
 * - Actual message passing (msgsnd/msgrcv).
 * - Actual semaphore operations (semop).
 * - Actual shared-memory mapping (shmat/shmdt).
 * These operations require kernel support that cannot be added by a module.
 * Treat this module as identity/lifecycle tracking that lets a patched runtime
 * proceed without failing on the resource-management calls.
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
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/hashtable.h>

#include "include/uapi/shadow_sysvipc.h"

#define SHADOW_SYSVIPC_MAX_RESOURCES	65536
#define SHADOW_SYSVIPC_KEY_HTBITS	8	/* 256 buckets */

/*
 * struct svipc_resource - a single virtual IPC object.
 * @id:        stable id handed to userspace (xa key)
 * @type:      enum shadow_sysvipc_type
 * @key:       application key, or SHADOW_IPC_PRIVATE
 * @flags:     permission bits stored at creation
 * @nsems:     semaphore count (TYPE_SEM only)
 * @size:      segment size (TYPE_SHM only)
 * @refcount:  reference count; dropped by each session reference
 * @key_node:  hash node in the global key table (only when key != PRIVATE)
 */
struct svipc_resource {
	u32			id;
	u32			type;
	s32			key;
	u32			flags;
	u32			nsems;
	u32			_pad;
	u64			size;
	refcount_t		refcount;
	struct hlist_node	key_node;
};

/*
 * struct svipc_session - per-open-fd state.
 * @owned: list of svipc_owned_ref tracking resources this fd created or got
 * @lock:  serialises all operations within a session
 */
struct svipc_session {
	struct list_head	owned;
	struct mutex		lock;
};

struct svipc_owned_ref {
	struct list_head	 node;
	struct svipc_resource	*res;
};

/* Global registry: id -> svipc_resource. */
static DEFINE_XARRAY_ALLOC1(svipc_map);
/* Protected by svipc_map_lock: key-hash and xa operations. */
static DEFINE_MUTEX(svipc_map_lock);
static DEFINE_HASHTABLE(svipc_key_hash, SHADOW_SYSVIPC_KEY_HTBITS);
static atomic_t svipc_count = ATOMIC_INIT(0);

static bool svipc_type_valid(u32 type)
{
	return type < SHADOW_SYSVIPC_TYPE_MAX;
}

static u32 svipc_key_hash_val(s32 key, u32 type)
{
	return (u32)key ^ (type << 28);
}

/*
 * Find a resource by key and type.  Caller must hold svipc_map_lock.
 * Returns a borrowed pointer (no refcount bump); caller must bump before
 * releasing the lock if it wants to keep the reference.
 */
static struct svipc_resource *svipc_find_key_locked(s32 key, u32 type)
{
	struct svipc_resource *res;
	u32 h = svipc_key_hash_val(key, type);

	hash_for_each_possible(svipc_key_hash, res, key_node, h) {
		if (res->key == key && res->type == type)
			return res;
	}
	return NULL;
}

/*
 * Get a reference to a resource by id.
 * Returns NULL if the id is unknown or the resource is being freed.
 */
static struct svipc_resource *svipc_get(u32 id)
{
	struct svipc_resource *res;

	if (!id)
		return NULL;
	mutex_lock(&svipc_map_lock);
	res = xa_load(&svipc_map, id);
	if (res && !refcount_inc_not_zero(&res->refcount))
		res = NULL;
	mutex_unlock(&svipc_map_lock);
	return res;
}

static void svipc_put(struct svipc_resource *res)
{
	if (!res)
		return;
	if (refcount_dec_and_test(&res->refcount)) {
		mutex_lock(&svipc_map_lock);
		if (res->key != SHADOW_IPC_PRIVATE)
			hash_del(&res->key_node);
		xa_erase(&svipc_map, res->id);
		mutex_unlock(&svipc_map_lock);
		atomic_dec(&svipc_count);
		kfree(res);
	}
}

/* Record a creation-or-get reference owned by this session's fd. */
static int svipc_session_own(struct svipc_session *s,
			     struct svipc_resource *res)
{
	struct svipc_owned_ref *ref;

	ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (!ref)
		return -ENOMEM;
	ref->res = res;
	list_add(&ref->node, &s->owned);
	return 0;
}

/* ---- ioctl handlers ---- */

static long svipc_ioc_create(struct svipc_session *s, void __user *arg)
{
	struct shadow_sysvipc_create req;
	struct svipc_resource *res = NULL;
	int ret;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (!svipc_type_valid(req.type))
		return -EINVAL;

	/*
	 * Keyed resources: find-or-create must be atomic so two concurrent
	 * callers with the same key do not both create new objects.
	 * Hold svipc_map_lock across the whole operation; kzalloc with
	 * GFP_KERNEL is safe while holding a mutex (not a spinlock).
	 */
	if (req.key != SHADOW_IPC_PRIVATE) {
		mutex_lock(&svipc_map_lock);

		res = svipc_find_key_locked(req.key, req.type);
		if (res) {
			/* Key already exists. */
			if ((req.flags & SHADOW_IPC_CREAT) &&
			    (req.flags & SHADOW_IPC_EXCL)) {
				mutex_unlock(&svipc_map_lock);
				return -EEXIST;
			}
			if (!refcount_inc_not_zero(&res->refcount))
				res = NULL; /* race: being freed; fall through */
		}

		if (!res) {
			/* Must create a new one. */
			if (!(req.flags & SHADOW_IPC_CREAT)) {
				mutex_unlock(&svipc_map_lock);
				return -ENOENT;
			}
			if (atomic_read(&svipc_count) >= SHADOW_SYSVIPC_MAX_RESOURCES) {
				mutex_unlock(&svipc_map_lock);
				return -ENOSPC;
			}

			res = kzalloc(sizeof(*res), GFP_KERNEL);
			if (!res) {
				mutex_unlock(&svipc_map_lock);
				return -ENOMEM;
			}
			res->type  = req.type;
			res->key   = req.key;
			res->flags = req.flags & 0777;
			res->nsems = req.nsems;
			res->size  = req.size;
			refcount_set(&res->refcount, 1);

			ret = xa_alloc(&svipc_map, &res->id, res,
				       XA_LIMIT(1, INT_MAX), GFP_KERNEL);
			if (ret) {
				mutex_unlock(&svipc_map_lock);
				kfree(res);
				return ret;
			}
			hash_add(svipc_key_hash, &res->key_node,
				 svipc_key_hash_val(req.key, req.type));
			atomic_inc(&svipc_count);
		}

		mutex_unlock(&svipc_map_lock);
	} else {
		/* Private resource: no key lookup needed. */
		if (atomic_read(&svipc_count) >= SHADOW_SYSVIPC_MAX_RESOURCES)
			return -ENOSPC;

		res = kzalloc(sizeof(*res), GFP_KERNEL);
		if (!res)
			return -ENOMEM;
		res->type  = req.type;
		res->key   = SHADOW_IPC_PRIVATE;
		res->flags = req.flags & 0777;
		res->nsems = req.nsems;
		res->size  = req.size;
		refcount_set(&res->refcount, 1);

		mutex_lock(&svipc_map_lock);
		ret = xa_alloc(&svipc_map, &res->id, res,
			       XA_LIMIT(1, INT_MAX), GFP_KERNEL);
		mutex_unlock(&svipc_map_lock);
		if (ret) {
			kfree(res);
			return ret;
		}
		atomic_inc(&svipc_count);
	}

	/* Record the reference in the session. */
	mutex_lock(&s->lock);
	ret = svipc_session_own(s, res);
	mutex_unlock(&s->lock);
	if (ret) {
		svipc_put(res);
		return ret;
	}

	req.id = res->id;
	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long svipc_ioc_stat(struct svipc_session *s, void __user *arg)
{
	struct shadow_sysvipc_stat req;
	struct svipc_resource *res;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (!svipc_type_valid(req.type))
		return -EINVAL;

	res = svipc_get(req.id);
	if (!res)
		return -EINVAL;
	if (res->type != req.type) {
		svipc_put(res);
		return -EINVAL;
	}

	req.key   = res->key;
	req.flags = res->flags;
	req.nsems = res->nsems;
	req.size  = res->size;
	svipc_put(res);

	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long svipc_ioc_destroy(struct svipc_session *s, void __user *arg)
{
	struct shadow_sysvipc_destroy req;
	struct svipc_owned_ref *ref, *tmp;
	bool found = false;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (!svipc_type_valid(req.type) || !req.id)
		return -EINVAL;

	mutex_lock(&s->lock);
	list_for_each_entry_safe(ref, tmp, &s->owned, node) {
		if (ref->res->id == req.id && ref->res->type == req.type) {
			list_del(&ref->node);
			svipc_put(ref->res);
			kfree(ref);
			found = true;
			break;
		}
	}
	mutex_unlock(&s->lock);
	return found ? 0 : -ENOENT;
}

static long svipc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct svipc_session *s = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case SHADOW_SYSVIPC_IOC_ABI_VERSION: {
		u32 ver = SHADOW_SYSVIPC_ABI_VERSION;

		return copy_to_user(uarg, &ver, sizeof(ver)) ? -EFAULT : 0;
	}
	case SHADOW_SYSVIPC_IOC_CREATE:
		return svipc_ioc_create(s, uarg);
	case SHADOW_SYSVIPC_IOC_STAT:
		return svipc_ioc_stat(s, uarg);
	case SHADOW_SYSVIPC_IOC_DESTROY:
		return svipc_ioc_destroy(s, uarg);
	default:
		return -ENOTTY;
	}
}

static int svipc_open(struct inode *inode, struct file *file)
{
	struct svipc_session *s;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	INIT_LIST_HEAD(&s->owned);
	mutex_init(&s->lock);
	file->private_data = s;
	return 0;
}

static int svipc_release(struct inode *inode, struct file *file)
{
	struct svipc_session *s = file->private_data;
	struct svipc_owned_ref *ref, *tmp;

	if (!s)
		return 0;

	list_for_each_entry_safe(ref, tmp, &s->owned, node) {
		list_del(&ref->node);
		svipc_put(ref->res);
		kfree(ref);
	}
	mutex_destroy(&s->lock);
	kfree(s);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations svipc_fops = {
	.owner		= THIS_MODULE,
	.open		= svipc_open,
	.release	= svipc_release,
	.unlocked_ioctl	= svipc_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= no_llseek,
};

static struct miscdevice svipc_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= SHADOW_SYSVIPC_DEVICE_NAME,
	.fops	= &svipc_fops,
	.mode	= 0600,
};

static int __init shadow_sysvipc_init(void)
{
	int ret;

	ret = misc_register(&svipc_miscdev);
	if (ret) {
		pr_err("shadow_sysvipc: failed to register misc device: %d\n", ret);
		return ret;
	}
	pr_info("shadow_sysvipc: simulated SysV IPC subsystem loaded (ABI v%d) at %s\n",
		SHADOW_SYSVIPC_ABI_VERSION, SHADOW_SYSVIPC_DEVICE_PATH);
	return 0;
}

static void __exit shadow_sysvipc_exit(void)
{
	struct svipc_resource *res;
	unsigned long id;

	misc_deregister(&svipc_miscdev);

	/*
	 * Free any objects that survived (e.g. leaked by a client that did not
	 * close its fd before rmmod).  The key-hash nodes are embedded in the
	 * resource structs so no separate cleanup is needed.
	 */
	mutex_lock(&svipc_map_lock);
	xa_for_each(&svipc_map, id, res) {
		xa_erase(&svipc_map, id);
		kfree(res);
	}
	mutex_unlock(&svipc_map_lock);
	xa_destroy(&svipc_map);

	pr_info("shadow_sysvipc: simulated SysV IPC subsystem unloaded\n");
}

module_init(shadow_sysvipc_init);
module_exit(shadow_sysvipc_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Simulated System V IPC subsystem for patched containerd");
MODULE_VERSION("1.0");
