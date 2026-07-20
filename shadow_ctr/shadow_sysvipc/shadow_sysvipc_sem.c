// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_sysvipc semaphore operations (semop/semtimedop), plus semctl(2)'s
 * value-manipulation commands (GETVAL/SETVAL/GETALL/SETALL).
 *
 * shadow_sysvipc_registry.c only tracks resource identity/bookkeeping (and,
 * for TYPE_SEM, the semaphore count via ->nsems); this file adds the actual
 * per-semaphore value array and the real semop(2) transaction semantics, so
 * unmodified callers relying on SysV semaphores (e.g. glibc's process-shared
 * robust mutex fallback, or any container workload doing its own SysV IPC
 * locking) get real synchronization instead of merely falling through to
 * -ENOSYS.
 *
 * Vendored semantics (see IEEE Std 1003.1 semop(2) / real ipc/sem.c):
 *   - Every op in the @sops array must be satisfiable as a single atomic
 *     transaction: for sem_op > 0 the semaphore's value is simply
 *     incremented (always succeeds); for sem_op == 0 the caller blocks
 *     until the semaphore's value is exactly 0; for sem_op < 0 the caller
 *     blocks until the semaphore's value is >= |sem_op|, then decrements it
 *     by |sem_op|. If *any* op in the array cannot proceed immediately, none
 *     of the ops are applied (IPC_NOWAIT then returns -EAGAIN; otherwise the
 *     caller blocks until a subsequent semop(2)/semctl(2) on this same
 *     resource changes a value, then the whole array is re-evaluated from
 *     scratch against the current values).
 *
 * Deliberately NOT vendored: SEM_UNDO (adjustment-on-exit bookkeeping) --
 * out of safe reach for a hook-based module the same way orphan reparenting
 * is for shadow_ns_pid.c's PID namespace simulation (see that file's own
 * header comment for the same class of trade-off): a process that dies
 * mid-critical-section while holding SEM_UNDO'd semaphores simply leaves
 * them at whatever value its own semop(2) calls last left them, instead of
 * having the kernel automatically roll the adjustment back. Every other
 * semop(2)/semctl(2) semantic is genuinely implemented.
 */
#include "shadow_sysvipc_internal.h"

/* Real SysV IPC's SEMVMX (ipc/sem.c) -- the largest value a single
 * semaphore may hold. Duplicated here verbatim since it isn't exported via
 * any uapi header.
 */
#define SVIPC_SEMVMX	32767

void svipc_sem_purge_locked(struct svipc_resource *res)
{
	kfree(res->sem_val);
	res->sem_val = NULL;
}

/*
 * svipc_sem_ops_fit_locked() - can every op in @sops be applied to @res's
 * current values without blocking? Caller must hold res->sem_lock.
 */
static bool svipc_sem_ops_fit_locked(struct svipc_resource *res,
				     struct sembuf *sops, unsigned int nsops)
{
	unsigned int i;

	for (i = 0; i < nsops; i++) {
		int val = res->sem_val[sops[i].sem_num];

		if (sops[i].sem_op > 0)
			continue;
		if (sops[i].sem_op == 0) {
			if (val != 0)
				return false;
		} else {
			if (val < -sops[i].sem_op)
				return false;
		}
	}
	return true;
}

/* Apply every op in @sops to @res's values. Caller must hold res->sem_lock
 * and must have already confirmed svipc_sem_ops_fit_locked() so this never
 * needs to roll back partway through.
 */
static void svipc_sem_ops_apply_locked(struct svipc_resource *res,
				       struct sembuf *sops, unsigned int nsops)
{
	unsigned int i;

	for (i = 0; i < nsops; i++) {
		int newval = res->sem_val[sops[i].sem_num] + sops[i].sem_op;

		if (newval > SVIPC_SEMVMX)
			newval = SVIPC_SEMVMX;
		if (newval < 0)
			newval = 0;
		res->sem_val[sops[i].sem_num] = newval;
	}
}

static bool svipc_sem_ops_valid(const struct svipc_resource *res,
				const struct sembuf *sops, unsigned int nsops)
{
	unsigned int i;

	for (i = 0; i < nsops; i++) {
		if (sops[i].sem_num >= res->nsems)
			return false;
	}
	return true;
}

long svipc_sys_semop(int semid, struct sembuf __user *tsops, unsigned int nsops,
		    const struct __kernel_timespec __user *utimeout)
{
	struct svipc_resource *res;
	struct sembuf stack_sops[8];
	struct sembuf *sops = stack_sops;
	long timeout_jiffies = MAX_SCHEDULE_TIMEOUT;
	bool nowait = false;
	long ret;
	unsigned int i;

	if (!nsops)
		return -EINVAL;
	if (nsops > SEMOPM)
		return -E2BIG;

	if (nsops > ARRAY_SIZE(stack_sops)) {
		sops = kmalloc_array(nsops, sizeof(*sops), GFP_KERNEL);
		if (!sops)
			return -ENOMEM;
	}
	if (copy_from_user(sops, tsops, nsops * sizeof(*sops))) {
		ret = -EFAULT;
		goto out_free;
	}

	if (utimeout) {
		struct __kernel_timespec ts;

		if (copy_from_user(&ts, utimeout, sizeof(ts))) {
			ret = -EFAULT;
			goto out_free;
		}
		if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= NSEC_PER_SEC) {
			ret = -EINVAL;
			goto out_free;
		}
		timeout_jiffies = timespec64_to_jiffies(
			&(struct timespec64){ .tv_sec = ts.tv_sec, .tv_nsec = ts.tv_nsec });
	}

	for (i = 0; i < nsops; i++) {
		if (sops[i].sem_flg & IPC_NOWAIT)
			nowait = true;
	}

	res = svipc_get(semid);
	if (!res) {
		ret = -EINVAL;
		goto out_free;
	}
	if (res->type != SHADOW_SYSVIPC_TYPE_SEM || !res->sem_val) {
		ret = -EINVAL;
		goto out_put;
	}
	if (!svipc_sem_ops_valid(res, sops, nsops)) {
		ret = -EFBIG;
		goto out_put;
	}

	for (;;) {
		mutex_lock(&res->sem_lock);
		if (svipc_sem_ops_fit_locked(res, sops, nsops)) {
			svipc_sem_ops_apply_locked(res, sops, nsops);
			mutex_unlock(&res->sem_lock);
			wake_up_interruptible_all(&res->sem_wait);
			ret = 0;
			goto out_put;
		}
		mutex_unlock(&res->sem_lock);

		if (nowait) {
			ret = -EAGAIN;
			goto out_put;
		}
		if (!utimeout) {
			ret = wait_event_interruptible(res->sem_wait,
					svipc_sem_ops_fit_locked(res, sops, nsops));
			if (ret) {
				ret = -ERESTARTSYS;
				goto out_put;
			}
			continue;
		}

		ret = wait_event_interruptible_timeout(res->sem_wait,
				svipc_sem_ops_fit_locked(res, sops, nsops),
				timeout_jiffies);
		if (ret == 0) {
			ret = -EAGAIN;
			goto out_put;
		}
		if (ret < 0) {
			ret = -ERESTARTSYS;
			goto out_put;
		}
		timeout_jiffies = ret;
	}

out_put:
	svipc_put(res);
out_free:
	if (sops != stack_sops)
		kfree(sops);
	return ret;
}

/*
 * svipc_sys_semctl_val() - the value-manipulation semctl(2) commands that
 * shadow_sysvipc_hooks.c's generic svipc_sys_semctl() (IPC_STAT/IPC_RMID)
 * does not itself handle.
 */
long svipc_sys_semctl_val(int semid, int semnum, int cmd, unsigned long arg)
{
	struct svipc_resource *res;
	long ret;
	unsigned int i;

	res = svipc_get(semid);
	if (!res)
		return -EINVAL;
	if (res->type != SHADOW_SYSVIPC_TYPE_SEM || !res->sem_val) {
		ret = -EINVAL;
		goto out;
	}

	switch (cmd) {
	case GETVAL:
		if (semnum < 0 || semnum >= res->nsems) {
			ret = -EINVAL;
			break;
		}
		mutex_lock(&res->sem_lock);
		ret = res->sem_val[semnum];
		mutex_unlock(&res->sem_lock);
		break;
	case SETVAL: {
		int val = (int)arg;

		if (semnum < 0 || semnum >= res->nsems) {
			ret = -EINVAL;
			break;
		}
		if (val < 0 || val > SVIPC_SEMVMX) {
			ret = -ERANGE;
			break;
		}
		mutex_lock(&res->sem_lock);
		res->sem_val[semnum] = val;
		mutex_unlock(&res->sem_lock);
		wake_up_interruptible_all(&res->sem_wait);
		ret = 0;
		break;
	}
	case GETALL: {
		unsigned short __user *uarr = (unsigned short __user *)arg;
		unsigned short *tmp;

		tmp = kmalloc_array(res->nsems, sizeof(*tmp), GFP_KERNEL);
		if (!tmp) {
			ret = -ENOMEM;
			break;
		}
		mutex_lock(&res->sem_lock);
		for (i = 0; i < res->nsems; i++)
			tmp[i] = (unsigned short)res->sem_val[i];
		mutex_unlock(&res->sem_lock);
		ret = copy_to_user(uarr, tmp, res->nsems * sizeof(*tmp)) ?
			-EFAULT : 0;
		kfree(tmp);
		break;
	}
	case SETALL: {
		unsigned short __user *uarr = (unsigned short __user *)arg;
		unsigned short *tmp;

		tmp = kmalloc_array(res->nsems, sizeof(*tmp), GFP_KERNEL);
		if (!tmp) {
			ret = -ENOMEM;
			break;
		}
		if (copy_from_user(tmp, uarr, res->nsems * sizeof(*tmp))) {
			kfree(tmp);
			ret = -EFAULT;
			break;
		}
		mutex_lock(&res->sem_lock);
		for (i = 0; i < res->nsems; i++)
			res->sem_val[i] = tmp[i];
		mutex_unlock(&res->sem_lock);
		wake_up_interruptible_all(&res->sem_wait);
		kfree(tmp);
		ret = 0;
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

out:
	svipc_put(res);
	return ret;
}
