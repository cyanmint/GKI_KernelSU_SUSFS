// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_sysvipc - simulated System V IPC subsystem
 *
 * A standalone loadable kernel module that provides bookkeeping for virtual
 * SysV IPC resources (message queues, semaphore sets, shared-memory segments)
 * through two entry paths:
 *   1. the original /dev/shadow_sysvipc misc-device ioctl ABI, and
 *   2. transparent ftrace hooks on the real msgget/msgctl/semget/... syscall
 *      wrappers so unmodified containerd/runc/dockerd can keep using the stock
 *      SysV IPC syscalls on kernels built with CONFIG_SYSVIPC=n.
 *
 * Motivation
 * ----------
 * The native SysV IPC machinery (CONFIG_SYSVIPC) is compiled into vmlinux.
 * It installs the msgget/msgsnd/msgrcv/semget/semop/shmget/shmat family of
 * syscalls and the /proc/sysvipc accounting.  None of that can be added by a
 * module after the kernel is built.
 *
 * shadow_sysvipc therefore implements only the resource-identity/bookkeeping
 * subset that is safe to synthesize in a module.  When the real syscall
 * wrappers merely fall through to sys_ni_syscall and return -ENOSYS, we
 * redirect selected operations into the same internal bookkeeping that backs
 * the ioctl API.
 *
 * What is simulated
 * -----------------
 * - Virtual resource objects with stable positive integer ids.
 * - Key-based lookup semantics mirroring msgget(2)/semget(2)/shmget(2).
 * - Reference counting tied to either open file descriptors (ioctl sessions) or
 *   task groups (transparent syscall path).
 * - msgctl/semctl/shmctl support for IPC_STAT and IPC_RMID on virtual objects.
 *
 * What is NOT simulated
 * ---------------------
 * - Actual message passing (msgsnd/msgrcv).
 * - Actual semaphore operations (semop/semtimedop).
 * - Actual shared-memory mapping (shmat/shmdt).
 * Those require real in-kernel SysV IPC data paths.  Returning a fabricated
 * success for them would be actively unsafe, so unsupported operations simply
 * preserve the kernel's existing behaviour (typically -ENOSYS on
 * CONFIG_SYSVIPC=n kernels).
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
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/sem.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <uapi/linux/shm.h>
#include <asm/ptrace.h>

#include "shadow_hook.h"
#include "shadow_ctr_internal.h"
#include "include/uapi/shadow_sysvipc.h"

#define SHADOW_SYSVIPC_MAX_RESOURCES	65536
#define SHADOW_SYSVIPC_KEY_HTBITS	8	/* 256 buckets */
#define SHADOW_SYSVIPC_TGID_HTBITS	8	/* 256 buckets */

/*
 * struct svipc_resource - a single virtual IPC object.
 * @id:        stable id handed to userspace (xa key)
 * @type:      enum shadow_sysvipc_type
 * @key:       application key, or SHADOW_IPC_PRIVATE
 * @flags:     permission bits stored at creation
 * @nsems:     semaphore count (TYPE_SEM only)
 * @size:      segment size (TYPE_SHM only)
 * @refcount:  reference count; dropped by each owner reference
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
	struct list_head	node;
	struct svipc_resource	*res;
};

/*
 * struct svipc_tgid_owner - per-task-group ownership state for hooked syscalls.
 * @tgid: thread-group id that owns @owned references
 * @owned: list of svipc_owned_ref entries mirroring the ioctl session model
 * @lock: serialises operations on @owned
 * @node: hash-table linkage
 *
 * Reaping is lazy rather than tracepoint-driven: on each transparent syscall
 * entry we opportunistically sweep the ownership table and drop entries whose
 * TGID no longer resolves to a live thread group.  This avoids sleeping/locking
 * concerns in sched_process_exit tracepoint context at the cost of leaked
 * bookkeeping surviving until the next intercepted SysV IPC syscall.
 */
struct svipc_tgid_owner {
	pid_t			 tgid;
	struct list_head	 owned;
	struct mutex		 lock;
	struct hlist_node	 node;
};

/* Global registry: id -> svipc_resource. */
static DEFINE_XARRAY_ALLOC1(svipc_map);
/* Protected by svipc_map_lock: key-hash and xa operations. */
static DEFINE_MUTEX(svipc_map_lock);
static DEFINE_HASHTABLE(svipc_key_hash, SHADOW_SYSVIPC_KEY_HTBITS);
static atomic_t svipc_count = ATOMIC_INIT(0);

/* Transparent syscall ownership registry: tgid -> svipc_tgid_owner. */
static DEFINE_MUTEX(svipc_tgid_lock);
static DEFINE_HASHTABLE(svipc_tgid_hash, SHADOW_SYSVIPC_TGID_HTBITS);

static long (*real_sys_msgget)(const struct pt_regs *regs);
static long (*real_sys_msgctl)(const struct pt_regs *regs);
static long (*real_sys_msgsnd)(const struct pt_regs *regs);
static long (*real_sys_msgrcv)(const struct pt_regs *regs);
static long (*real_sys_semget)(const struct pt_regs *regs);
static long (*real_sys_semctl)(const struct pt_regs *regs);
static long (*real_sys_semop)(const struct pt_regs *regs);
static long (*real_sys_semtimedop)(const struct pt_regs *regs);
static long (*real_sys_shmget)(const struct pt_regs *regs);
static long (*real_sys_shmctl)(const struct pt_regs *regs);
static long (*real_sys_shmat)(const struct pt_regs *regs);
static long (*real_sys_shmdt)(const struct pt_regs *regs);

static long svipc_hook_msgget(const struct pt_regs *regs);
static long svipc_hook_msgctl(const struct pt_regs *regs);
static long svipc_hook_msgsnd(const struct pt_regs *regs);
static long svipc_hook_msgrcv(const struct pt_regs *regs);
static long svipc_hook_semget(const struct pt_regs *regs);
static long svipc_hook_semctl(const struct pt_regs *regs);
static long svipc_hook_semop(const struct pt_regs *regs);
static long svipc_hook_semtimedop(const struct pt_regs *regs);
static long svipc_hook_shmget(const struct pt_regs *regs);
static long svipc_hook_shmctl(const struct pt_regs *regs);
static long svipc_hook_shmat(const struct pt_regs *regs);
static long svipc_hook_shmdt(const struct pt_regs *regs);

static unsigned long svipc_sys_arg(const struct pt_regs *regs, unsigned int n)
{
	return regs_get_kernel_argument((struct pt_regs *)regs, n);
}

static const char * const msgget_names[] = { "__arm64_sys_msgget", "sys_msgget", NULL };
static const char * const msgctl_names[] = { "__arm64_sys_msgctl", "sys_msgctl", NULL };
static const char * const msgsnd_names[] = { "__arm64_sys_msgsnd", "sys_msgsnd", NULL };
static const char * const msgrcv_names[] = { "__arm64_sys_msgrcv", "sys_msgrcv", NULL };
static const char * const semget_names[] = { "__arm64_sys_semget", "sys_semget", NULL };
static const char * const semctl_names[] = { "__arm64_sys_semctl", "sys_semctl", NULL };
static const char * const semop_names[] = { "__arm64_sys_semop", "sys_semop", NULL };
static const char * const semtimedop_names[] = { "__arm64_sys_semtimedop", "sys_semtimedop", NULL };
static const char * const shmget_names[] = { "__arm64_sys_shmget", "sys_shmget", NULL };
static const char * const shmctl_names[] = { "__arm64_sys_shmctl", "sys_shmctl", NULL };
static const char * const shmat_names[] = { "__arm64_sys_shmat", "sys_shmat", NULL };
static const char * const shmdt_names[] = { "__arm64_sys_shmdt", "sys_shmdt", NULL };

static struct shadow_hook msgget_hook =
	SHADOW_HOOK(msgget_names, svipc_hook_msgget, &real_sys_msgget);
static struct shadow_hook msgctl_hook =
	SHADOW_HOOK(msgctl_names, svipc_hook_msgctl, &real_sys_msgctl);
static struct shadow_hook msgsnd_hook =
	SHADOW_HOOK(msgsnd_names, svipc_hook_msgsnd, &real_sys_msgsnd);
static struct shadow_hook msgrcv_hook =
	SHADOW_HOOK(msgrcv_names, svipc_hook_msgrcv, &real_sys_msgrcv);
static struct shadow_hook semget_hook =
	SHADOW_HOOK(semget_names, svipc_hook_semget, &real_sys_semget);
static struct shadow_hook semctl_hook =
	SHADOW_HOOK(semctl_names, svipc_hook_semctl, &real_sys_semctl);
static struct shadow_hook semop_hook =
	SHADOW_HOOK(semop_names, svipc_hook_semop, &real_sys_semop);
static struct shadow_hook semtimedop_hook =
	SHADOW_HOOK(semtimedop_names, svipc_hook_semtimedop, &real_sys_semtimedop);
static struct shadow_hook shmget_hook =
	SHADOW_HOOK(shmget_names, svipc_hook_shmget, &real_sys_shmget);
static struct shadow_hook shmctl_hook =
	SHADOW_HOOK(shmctl_names, svipc_hook_shmctl, &real_sys_shmctl);
static struct shadow_hook shmat_hook =
	SHADOW_HOOK(shmat_names, svipc_hook_shmat, &real_sys_shmat);
static struct shadow_hook shmdt_hook =
	SHADOW_HOOK(shmdt_names, svipc_hook_shmdt, &real_sys_shmdt);

static struct shadow_hook *svipc_all_hooks[] = {
	&msgget_hook,
	&msgctl_hook,
	&msgsnd_hook,
	&msgrcv_hook,
	&semget_hook,
	&semctl_hook,
	&semop_hook,
	&semtimedop_hook,
	&shmget_hook,
	&shmctl_hook,
	&shmat_hook,
	&shmdt_hook,
	NULL,
};

static bool svipc_type_valid(u32 type)
{
	return type < SHADOW_SYSVIPC_TYPE_MAX;
}

static u32 svipc_key_hash_val(s32 key, u32 type)
{
	return (u32)key ^ (type << 28);
}

static u32 svipc_shadow_flags_from_ipc(int flags)
{
	u32 shadow = flags & 0777;

	if (flags & IPC_CREAT)
		shadow |= SHADOW_IPC_CREAT;
	if (flags & IPC_EXCL)
		shadow |= SHADOW_IPC_EXCL;
	return shadow;
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

static void svipc_fill_stat_from_res(struct shadow_sysvipc_stat *stat,
					  const struct svipc_resource *res)
{
	stat->key = res->key;
	stat->flags = res->flags;
	stat->nsems = res->nsems;
	stat->size = res->size;
}

static int svipc_resource_create_or_get(u32 type, s32 key, u32 flags,
					u32 nsems, u64 size,
					struct svipc_resource **res_out)
{
	struct svipc_resource *res = NULL;
	int ret;

	if (!svipc_type_valid(type))
		return -EINVAL;

	/*
	 * Keyed resources: find-or-create must be atomic so two concurrent callers
	 * with the same key do not both create new objects.
	 */
	if (key != SHADOW_IPC_PRIVATE) {
		mutex_lock(&svipc_map_lock);

		res = svipc_find_key_locked(key, type);
		if (res) {
			if ((flags & SHADOW_IPC_CREAT) && (flags & SHADOW_IPC_EXCL)) {
				mutex_unlock(&svipc_map_lock);
				return -EEXIST;
			}
			if (!refcount_inc_not_zero(&res->refcount))
				res = NULL;
		}

		if (!res) {
			if (!(flags & SHADOW_IPC_CREAT)) {
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
			res->type = type;
			res->key = key;
			res->flags = flags & 0777;
			res->nsems = nsems;
			res->size = size;
			refcount_set(&res->refcount, 1);

			ret = xa_alloc(&svipc_map, &res->id, res,
				       XA_LIMIT(1, INT_MAX), GFP_KERNEL);
			if (ret) {
				mutex_unlock(&svipc_map_lock);
				kfree(res);
				return ret;
			}
			hash_add(svipc_key_hash, &res->key_node,
				 svipc_key_hash_val(key, type));
			atomic_inc(&svipc_count);
		}

		mutex_unlock(&svipc_map_lock);
	} else {
		if (atomic_read(&svipc_count) >= SHADOW_SYSVIPC_MAX_RESOURCES)
			return -ENOSPC;

		res = kzalloc(sizeof(*res), GFP_KERNEL);
		if (!res)
			return -ENOMEM;
		res->type = type;
		res->key = SHADOW_IPC_PRIVATE;
		res->flags = flags & 0777;
		res->nsems = nsems;
		res->size = size;
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

	*res_out = res;
	return 0;
}

static int svipc_resource_stat(u32 type, u32 id, struct shadow_sysvipc_stat *stat)
{
	struct svipc_resource *res;

	if (!svipc_type_valid(type) || !id)
		return -EINVAL;

	res = svipc_get(id);
	if (!res)
		return -EINVAL;
	if (res->type != type) {
		svipc_put(res);
		return -EINVAL;
	}

	svipc_fill_stat_from_res(stat, res);
	svipc_put(res);
	return 0;
}

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

static void svipc_tgid_reap_dead(void)
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

static int svipc_tgid_own_current(struct svipc_resource *res)
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

static int svipc_tgid_destroy_current(u32 type, u32 id)
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

static void svipc_tgid_release_all(void)
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

static void svipc_force_free_all_resources(void)
{
	struct svipc_resource *res;
	unsigned long id;

	mutex_lock(&svipc_map_lock);
	xa_for_each(&svipc_map, id, res) {
		if (res->key != SHADOW_IPC_PRIVATE)
			hash_del(&res->key_node);
		xa_erase(&svipc_map, id);
		kfree(res);
	}
	mutex_unlock(&svipc_map_lock);
	xa_destroy(&svipc_map);
	atomic_set(&svipc_count, 0);
}

/* ---- ioctl handlers ---- */

static long svipc_ioc_create(struct svipc_session *s, void __user *arg)
{
	struct shadow_sysvipc_create req;
	struct svipc_resource *res;
	int ret;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	ret = svipc_resource_create_or_get(req.type, req.key, req.flags,
					   req.nsems, req.size, &res);
	if (ret)
		return ret;

	mutex_lock(&s->lock);
	ret = svipc_owned_ref_add_locked(&s->owned, res);
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
	int ret;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	ret = svipc_resource_stat(req.type, req.id, &req);
	if (ret)
		return ret;

	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long svipc_ioc_destroy(struct svipc_session *s, void __user *arg)
{
	struct shadow_sysvipc_destroy req;
	int ret;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (!svipc_type_valid(req.type) || !req.id)
		return -EINVAL;

	mutex_lock(&s->lock);
	ret = svipc_owned_ref_destroy_locked(&s->owned, req.type, req.id);
	mutex_unlock(&s->lock);
	return ret;
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

	if (!s)
		return 0;

	mutex_lock(&s->lock);
	svipc_owned_ref_release_all_locked(&s->owned);
	mutex_unlock(&s->lock);
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
	.llseek		= noop_llseek,
};

static struct miscdevice svipc_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= SHADOW_SYSVIPC_DEVICE_NAME,
	.fops	= &svipc_fops,
	.mode	= 0600,
};

/* ---- transparent syscall helpers ---- */

static long svipc_sys_create(u32 type, s32 key, int flags, u32 nsems, u64 size)
{
	struct svipc_resource *res;
	int ret;

	svipc_tgid_reap_dead();

	ret = svipc_resource_create_or_get(type, key,
					   svipc_shadow_flags_from_ipc(flags),
					   nsems, size, &res);
	if (ret)
		return ret;

	ret = svipc_tgid_own_current(res);
	if (ret) {
		svipc_put(res);
		return ret;
	}

	return (long)res->id;
}

static int svipc_msgctl_stat_to_user(int msqid, void __user *arg)
{
	struct shadow_sysvipc_stat stat = {
		.type = SHADOW_SYSVIPC_TYPE_MSGQ,
		.id = msqid,
	};
	struct msqid_ds ds;
	int ret;

	ret = svipc_resource_stat(stat.type, stat.id, &stat);
	if (ret)
		return ret;

	memset(&ds, 0, sizeof(ds));
	ds.msg_perm.key = stat.key;
	ds.msg_perm.mode = stat.flags & 0777;
	ds.msg_qbytes = MSGMNB;

	return copy_to_user(arg, &ds, sizeof(ds)) ? -EFAULT : 0;
}

static int svipc_semctl_stat_to_user(int semid, void __user *arg)
{
	struct shadow_sysvipc_stat stat = {
		.type = SHADOW_SYSVIPC_TYPE_SEM,
		.id = semid,
	};
	struct semid_ds ds;
	int ret;

	ret = svipc_resource_stat(stat.type, stat.id, &stat);
	if (ret)
		return ret;

	memset(&ds, 0, sizeof(ds));
	ds.sem_perm.key = stat.key;
	ds.sem_perm.mode = stat.flags & 0777;
	ds.sem_nsems = stat.nsems;

	return copy_to_user(arg, &ds, sizeof(ds)) ? -EFAULT : 0;
}

static int svipc_shmctl_stat_to_user(int shmid, void __user *arg)
{
	struct shadow_sysvipc_stat stat = {
		.type = SHADOW_SYSVIPC_TYPE_SHM,
		.id = shmid,
	};
	struct shmid_ds ds;
	int ret;

	ret = svipc_resource_stat(stat.type, stat.id, &stat);
	if (ret)
		return ret;

	memset(&ds, 0, sizeof(ds));
	ds.shm_perm.key = stat.key;
	ds.shm_perm.mode = stat.flags & 0777;
	ds.shm_segsz = stat.size;

	return copy_to_user(arg, &ds, sizeof(ds)) ? -EFAULT : 0;
}

static long svipc_sys_msgctl(int msqid, int cmd, void __user *arg)
{
	svipc_tgid_reap_dead();

	switch (cmd & ~IPC_64) {
	case IPC_RMID:
		return svipc_tgid_destroy_current(SHADOW_SYSVIPC_TYPE_MSGQ, msqid);
	case IPC_STAT:
		return svipc_msgctl_stat_to_user(msqid, arg);
	default:
		return -ENOSYS;
	}
}

static long svipc_sys_semctl(int semid, int semnum, int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	(void)semnum;
	svipc_tgid_reap_dead();

	switch (cmd & ~IPC_64) {
	case IPC_RMID:
		return svipc_tgid_destroy_current(SHADOW_SYSVIPC_TYPE_SEM, semid);
	case IPC_STAT:
		return svipc_semctl_stat_to_user(semid, uarg);
	default:
		return -ENOSYS;
	}
}

static long svipc_sys_shmctl(int shmid, int cmd, void __user *arg)
{
	svipc_tgid_reap_dead();

	switch (cmd & ~IPC_64) {
	case IPC_RMID:
		return svipc_tgid_destroy_current(SHADOW_SYSVIPC_TYPE_SHM, shmid);
	case IPC_STAT:
		return svipc_shmctl_stat_to_user(shmid, arg);
	default:
		return -ENOSYS;
	}
}

/* ---- hooked syscall wrappers ---- */

static long svipc_hook_msgget(const struct pt_regs *regs)
{
	long ret;
	s32 key = (s32)svipc_sys_arg(regs, 0);
	int msgflg = (int)svipc_sys_arg(regs, 1);

	ret = real_sys_msgget(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_create(SHADOW_SYSVIPC_TYPE_MSGQ, key, msgflg, 0, 0);
}

static long svipc_hook_msgctl(const struct pt_regs *regs)
{
	long ret;
	int msqid = (int)svipc_sys_arg(regs, 0);
	int cmd = (int)svipc_sys_arg(regs, 1);
	void __user *buf = (void __user *)svipc_sys_arg(regs, 2);

	ret = real_sys_msgctl(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_msgctl(msqid, cmd, buf);
}

static long svipc_hook_msgsnd(const struct pt_regs *regs)
{
	/*
	 * No safe shadow data path exists here yet: a real implementation needs an
	 * in-kernel message queue, payload storage, blocking/wakeup semantics and
	 * careful copy_{from,to}_user handling.  Preserve native behaviour instead.
	 */
	return real_sys_msgsnd(regs);
}

static long svipc_hook_msgrcv(const struct pt_regs *regs)
{
	/* See svipc_hook_msgsnd(): bookkeeping-only shadow queues cannot safely fake
	 * successful payload transfer semantics.
	 */
	return real_sys_msgrcv(regs);
}

static long svipc_hook_semget(const struct pt_regs *regs)
{
	long ret;
	s32 key = (s32)svipc_sys_arg(regs, 0);
	int nsems = (int)svipc_sys_arg(regs, 1);
	int semflg = (int)svipc_sys_arg(regs, 2);

	ret = real_sys_semget(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_create(SHADOW_SYSVIPC_TYPE_SEM, key, semflg,
				 nsems, 0);
}

static long svipc_hook_semctl(const struct pt_regs *regs)
{
	long ret;
	int semid = (int)svipc_sys_arg(regs, 0);
	int semnum = (int)svipc_sys_arg(regs, 1);
	int cmd = (int)svipc_sys_arg(regs, 2);
	unsigned long arg = svipc_sys_arg(regs, 3);

	ret = real_sys_semctl(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_semctl(semid, semnum, cmd, arg);
}

static long svipc_hook_semop(const struct pt_regs *regs)
{
	/*
	 * Real SysV semaphore transactions require atomic multi-op semantics,
	 * waiting rules, wakeups and SEM_UNDO bookkeeping.  A partial imitation is
	 * too easy to get subtly wrong without a real kernel test matrix.
	 */
	return real_sys_semop(regs);
}

static long svipc_hook_semtimedop(const struct pt_regs *regs)
{
	return real_sys_semtimedop(regs);
}

static long svipc_hook_shmget(const struct pt_regs *regs)
{
	long ret;
	s32 key = (s32)svipc_sys_arg(regs, 0);
	size_t size = (size_t)svipc_sys_arg(regs, 1);
	int shmflg = (int)svipc_sys_arg(regs, 2);

	ret = real_sys_shmget(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_create(SHADOW_SYSVIPC_TYPE_SHM, key, shmflg, 0, size);
}

static long svipc_hook_shmctl(const struct pt_regs *regs)
{
	long ret;
	int shmid = (int)svipc_sys_arg(regs, 0);
	int cmd = (int)svipc_sys_arg(regs, 1);
	void __user *buf = (void __user *)svipc_sys_arg(regs, 2);

	ret = real_sys_shmctl(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_shmctl(shmid, cmd, buf);
}

static long svipc_hook_shmat(const struct pt_regs *regs)
{
	/*
	 * Intentionally unimplemented.  Fabricating a successful attach without a
	 * real backing VM object would hand userspace a bogus pointer and risk memory
	 * corruption, so we preserve the kernel's existing result (-ENOSYS on stubbed
	 * kernels, native behaviour on CONFIG_SYSVIPC=y kernels).
	 */
	return real_sys_shmat(regs);
}

static long svipc_hook_shmdt(const struct pt_regs *regs)
{
	return real_sys_shmdt(regs);
}

int __init shadow_sysvipc_init(void)
{
	int ret;

	pr_info("shadow_sysvipc: init: registering misc device %s\n",
		SHADOW_SYSVIPC_DEVICE_PATH);
	ret = misc_register(&svipc_miscdev);
	if (ret) {
		pr_err("shadow_sysvipc: failed to register misc device: %d\n", ret);
		return ret;
	}
	pr_info("shadow_sysvipc: init: misc device registered\n");

	pr_info("shadow_sysvipc: init: installing transparent syscall hooks\n");
	ret = shadow_hook_install_all(svipc_all_hooks, "shadow_sysvipc");
	if (ret < 0) {
		pr_err("shadow_sysvipc: init: shadow_hook_install_all() failed: %d\n", ret);
		shadow_hook_remove_all(svipc_all_hooks);
		misc_deregister(&svipc_miscdev);
		return ret;
	}
	pr_info("shadow_sysvipc: init: %d hook(s) installed\n", ret);

	pr_info("shadow_sysvipc: simulated SysV IPC subsystem loaded (ABI v%d) at %s\n",
		SHADOW_SYSVIPC_ABI_VERSION, SHADOW_SYSVIPC_DEVICE_PATH);
	return 0;
}

void shadow_sysvipc_exit(void)
{
	pr_info("shadow_sysvipc: exit: removing transparent syscall hooks\n");
	shadow_hook_remove_all(svipc_all_hooks);
	pr_info("shadow_sysvipc: exit: deregistering misc device\n");
	misc_deregister(&svipc_miscdev);

	pr_info("shadow_sysvipc: exit: releasing per-tgid state\n");
	svipc_tgid_release_all();
	pr_info("shadow_sysvipc: exit: force-freeing remaining resources\n");
	svipc_force_free_all_resources();

	pr_info("shadow_sysvipc: simulated SysV IPC subsystem unloaded\n");
}
