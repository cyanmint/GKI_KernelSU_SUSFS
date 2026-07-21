// SPDX-License-Identifier: GPL-2.0
#include "shadow_sysvipc_internal.h"
#include "lkm4ctr_log.h"

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
	case GETVAL:
	case SETVAL:
	case GETALL:
	case SETALL:
		return svipc_sys_semctl_val(semid, semnum, cmd & ~IPC_64, arg);
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
	long ret;
	int msqid = (int)svipc_sys_arg(regs, 0);
	const void __user *umsgp = (const void __user *)svipc_sys_arg(regs, 1);
	size_t msgsz = (size_t)svipc_sys_arg(regs, 2);
	int msgflg = (int)svipc_sys_arg(regs, 3);

	ret = real_sys_msgsnd(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_msgsnd(msqid, umsgp, msgsz, msgflg);
}

static long svipc_hook_msgrcv(const struct pt_regs *regs)
{
	long ret;
	int msqid = (int)svipc_sys_arg(regs, 0);
	void __user *umsgp = (void __user *)svipc_sys_arg(regs, 1);
	size_t msgsz = (size_t)svipc_sys_arg(regs, 2);
	long msgtyp = (long)svipc_sys_arg(regs, 3);
	int msgflg = (int)svipc_sys_arg(regs, 4);

	ret = real_sys_msgrcv(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_msgrcv(msqid, umsgp, msgsz, msgtyp, msgflg);
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
	long ret;
	int semid = (int)svipc_sys_arg(regs, 0);
	struct sembuf __user *tsops = (struct sembuf __user *)svipc_sys_arg(regs, 1);
	unsigned int nsops = (unsigned int)svipc_sys_arg(regs, 2);

	ret = real_sys_semop(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_semop(semid, tsops, nsops, NULL);
}

static long svipc_hook_semtimedop(const struct pt_regs *regs)
{
	long ret;
	int semid = (int)svipc_sys_arg(regs, 0);
	struct sembuf __user *tsops = (struct sembuf __user *)svipc_sys_arg(regs, 1);
	unsigned int nsops = (unsigned int)svipc_sys_arg(regs, 2);
	const struct __kernel_timespec __user *utimeout =
		(const struct __kernel_timespec __user *)svipc_sys_arg(regs, 3);

	ret = real_sys_semtimedop(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_semop(semid, tsops, nsops, utimeout);
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
	long ret;
	int shmid = (int)svipc_sys_arg(regs, 0);
	const void __user *ushmaddr = (const void __user *)svipc_sys_arg(regs, 1);
	int shmflg = (int)svipc_sys_arg(regs, 2);
	unsigned long raddr = 0;

	ret = real_sys_shmat(regs);
	if (ret != -ENOSYS)
		return ret;

	ret = svipc_sys_shmat(shmid, ushmaddr, shmflg, &raddr);
	if (ret < 0)
		return ret;
	return (long)raddr;
}

static long svipc_hook_shmdt(const struct pt_regs *regs)
{
	long ret;
	const void __user *ushmaddr = (const void __user *)svipc_sys_arg(regs, 0);

	ret = real_sys_shmdt(regs);
	if (ret != -ENOSYS)
		return ret;

	return svipc_sys_shmdt(ushmaddr);
}

int shadow_sysvipc_init(void)
{
	int ret;

	LKM4CTR_INFO("shadow_sysvipc", "init: installing transparent syscall hooks");
	ret = shadow_hook_install_all(svipc_all_hooks, "shadow_sysvipc");
	if (ret < 0) {
		LKM4CTR_ERR("shadow_sysvipc", "init: shadow_hook_install_all() failed: %d", ret);
		shadow_hook_remove_all(svipc_all_hooks);
		return ret;
	}
	LKM4CTR_INFO("shadow_sysvipc", "init: %d hook(s) installed", ret);

	LKM4CTR_INFO("shadow_sysvipc", "simulated SysV IPC subsystem loaded with transparent syscall hooks");
	return 0;
}

void shadow_sysvipc_exit(void)
{
	LKM4CTR_INFO("shadow_sysvipc", "exit: removing transparent syscall hooks");
	shadow_hook_remove_all(svipc_all_hooks);

	LKM4CTR_INFO("shadow_sysvipc", "exit: releasing per-tgid state");
	svipc_tgid_release_all();
	LKM4CTR_INFO("shadow_sysvipc", "exit: force-freeing remaining resources");
	svipc_force_free_all_resources();

	LKM4CTR_INFO("shadow_sysvipc", "simulated SysV IPC subsystem unloaded");
}

/*
 * Presence marker retained for any in-kernel consumer that wants to detect
 * whether the SysV IPC subsystem is present inside lkm4ctr.ko. Returns the
 * module ABI/version tag.
 */
int shadow_sysvipc_is_active(void)
{
	return 1;
}
EXPORT_SYMBOL_GPL(shadow_sysvipc_is_active);
