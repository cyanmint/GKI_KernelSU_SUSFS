// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_sysvipc - simulated System V IPC subsystem
 *
 * The shadow_ctr.ko SysV IPC subsystem provides bookkeeping for virtual
 * SysV IPC resources (message queues, semaphore sets, shared-memory segments)
 * through transparent ftrace hooks on the real msgget/msgctl/semget/...
 * syscall wrappers so unmodified containerd/runc/dockerd can keep using the
 * stock SysV IPC syscalls on kernels built with CONFIG_SYSVIPC=n.
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
 * redirect selected operations into shadow-managed bookkeeping.
 *
 * What is simulated
 * -----------------
 * - Virtual resource objects with stable positive integer ids.
 * - Key-based lookup semantics mirroring msgget(2)/semget(2)/shmget(2).
 * - Reference counting tied to task groups (transparent syscall path).
 * - msgctl/semctl/shmctl support for IPC_STAT and IPC_RMID on virtual objects.
 * - Real message-queue payload transfer (msgsnd/msgrcv), including mtype
 *   matching, blocking, IPC_NOWAIT and MSG_NOERROR semantics (see
 *   shadow_sysvipc_msgq.c).
 *
 * What is NOT simulated
 * ---------------------
 * - Actual semaphore operations (semop/semtimedop).
 * - Actual shared-memory mapping (shmat/shmdt).
 * Those require real in-kernel SysV IPC data paths.  Returning a fabricated
 * success for them would be actively unsafe, so unsupported operations simply
 * preserve the kernel's existing behaviour (typically -ENOSYS on
 * CONFIG_SYSVIPC=n kernels).
 */

#include "shadow_sysvipc_internal.h"

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
 * svipc_current_ns_id() - the simulated IPC namespace id key-based
 * msgget(2)/semget(2)/shmget(2) lookups (svipc_find_key_locked() below)
 * should be scoped to for the calling task, mirroring the *shape* of real
 * ipc/namespace.c's per-namespace registries (copy_ipcs()/free_ipcs()):
 * each simulated IPC namespace gets its own independent key -> resource
 * mapping, so two different containers both requesting, say,
 * msgget(0x1234, IPC_CREAT) do not collide with each other's queue the way
 * a single flat global key table would. Resources are still allocated a
 * single global id (svipc_map's xarray key) exactly as before: real SysV
 * ids are likewise unique kernel-wide, not per-namespace, so this only
 * needed to change for the *key* lookup path, not id-based svipc_get().
 */
static u32 svipc_current_ns_id(void)
{
	return shadow_ns_current_ipc_ns_id();
}

static void svipc_resource_init_common(struct svipc_resource *res)
{
	INIT_LIST_HEAD(&res->msgs);
	mutex_init(&res->msgs_lock);
	init_waitqueue_head(&res->msgs_wait);
	mutex_init(&res->sem_lock);
	init_waitqueue_head(&res->sem_wait);
	mutex_init(&res->shm_lock);
}

/*
 * svipc_resource_alloc_sem_val() - allocate the per-semaphore value array
 * for a freshly created TYPE_SEM resource, mirroring semget(2)'s own
 * "all values initialised to 0" rule (real ipc/sem.c's sem_alloc()).
 * No-op (and never fails) for any other resource type.
 */
static int svipc_resource_alloc_sem_val(struct svipc_resource *res)
{
	if (res->type != SHADOW_SYSVIPC_TYPE_SEM || !res->nsems)
		return 0;
	res->sem_val = kcalloc(res->nsems, sizeof(*res->sem_val), GFP_KERNEL);
	return res->sem_val ? 0 : -ENOMEM;
}

u32 svipc_shadow_flags_from_ipc(int flags)
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
	u32 ns_id = svipc_current_ns_id();

	hash_for_each_possible(svipc_key_hash, res, key_node, h) {
		if (res->key == key && res->type == type && res->ns_id == ns_id)
			return res;
	}
	return NULL;
}

/*
 * Get a reference to a resource by id.
 * Returns NULL if the id is unknown or the resource is being freed.
 */
struct svipc_resource *svipc_get(u32 id)
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

void svipc_put(struct svipc_resource *res)
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
		if (res->type == SHADOW_SYSVIPC_TYPE_MSGQ)
			svipc_msgq_purge_locked(res);
		if (res->type == SHADOW_SYSVIPC_TYPE_SEM)
			svipc_sem_purge_locked(res);
		if (res->type == SHADOW_SYSVIPC_TYPE_SHM)
			svipc_shm_purge_locked(res);
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

int svipc_resource_create_or_get(u32 type, s32 key, u32 flags,
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
			res->ns_id = svipc_current_ns_id();
			refcount_set(&res->refcount, 1);
			svipc_resource_init_common(res);
			if (svipc_resource_alloc_sem_val(res)) {
				mutex_unlock(&svipc_map_lock);
				kfree(res);
				return -ENOMEM;
			}

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
		svipc_resource_init_common(res);
		if (svipc_resource_alloc_sem_val(res)) {
			kfree(res);
			return -ENOMEM;
		}

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

int svipc_resource_stat(u32 type, u32 id, struct shadow_sysvipc_stat *stat)
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

void svipc_force_free_all_resources(void)
{
	struct svipc_resource *res;
	unsigned long id;

	mutex_lock(&svipc_map_lock);
	xa_for_each(&svipc_map, id, res) {
		if (res->key != SHADOW_IPC_PRIVATE)
			hash_del(&res->key_node);
		xa_erase(&svipc_map, id);
		if (res->type == SHADOW_SYSVIPC_TYPE_MSGQ)
			svipc_msgq_purge_locked(res);
		if (res->type == SHADOW_SYSVIPC_TYPE_SEM)
			svipc_sem_purge_locked(res);
		if (res->type == SHADOW_SYSVIPC_TYPE_SHM)
			svipc_shm_purge_locked(res);
		kfree(res);
	}
	mutex_unlock(&svipc_map_lock);
	xa_destroy(&svipc_map);
	atomic_set(&svipc_count, 0);
}
