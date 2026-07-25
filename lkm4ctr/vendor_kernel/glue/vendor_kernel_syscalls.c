// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_syscalls.c - syscall hook handlers for vendor_kernel.
 * This is NEW code (not vendored from kernel-common).
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/mount.h>
#include <asm/ptrace.h>

#include "../vendor_kernel.h"
#include "../../../common/shadow_hook.h"

static long (*real_sys_unshare)(const struct pt_regs *regs);
static long (*real_sys_setns)(const struct pt_regs *regs);
static long (*real_sys_clone)(const struct pt_regs *regs);
static long (*real_sys_clone3)(const struct pt_regs *regs);
static long (*real_sys_fork)(const struct pt_regs *regs);
static long (*real_sys_vfork)(const struct pt_regs *regs);
#ifdef CONFIG_POSIX_MQUEUE
static long (*real_sys_mq_open)(const struct pt_regs *regs);
static long (*real_sys_mq_unlink)(const struct pt_regs *regs);
static long (*real_sys_mq_timedsend)(const struct pt_regs *regs);
static long (*real_sys_mq_timedreceive)(const struct pt_regs *regs);
static long (*real_sys_mq_notify)(const struct pt_regs *regs);
static long (*real_sys_mq_getsetattr)(const struct pt_regs *regs);
static long (*real_sys_mount)(const struct pt_regs *regs);
#endif
#ifdef CONFIG_SYSVIPC
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
#endif

static const char * const vendor_kernel_unshare_names[] = {
	"__arm64_sys_unshare", "__x64_sys_unshare", "sys_unshare", NULL,
};
static const char * const vendor_kernel_setns_names[] = {
	"__arm64_sys_setns", "__x64_sys_setns", "sys_setns", NULL,
};
static const char * const vendor_kernel_clone_names[] = {
	"__arm64_sys_clone", "__x64_sys_clone", "sys_clone", NULL,
};
static const char * const vendor_kernel_clone3_names[] = {
	"__arm64_sys_clone3", "__x64_sys_clone3", "sys_clone3", NULL,
};
static const char * const vendor_kernel_fork_names[] = {
	"__arm64_sys_fork", "__x64_sys_fork", "sys_fork", NULL,
};
static const char * const vendor_kernel_vfork_names[] = {
	"__arm64_sys_vfork", "__x64_sys_vfork", "sys_vfork", NULL,
};
#ifdef CONFIG_POSIX_MQUEUE
static const char * const vendor_kernel_mq_open_names[] = {
	"__arm64_sys_mq_open", "__x64_sys_mq_open", "sys_mq_open", NULL,
};
static const char * const vendor_kernel_mq_unlink_names[] = {
	"__arm64_sys_mq_unlink", "__x64_sys_mq_unlink", "sys_mq_unlink", NULL,
};
static const char * const vendor_kernel_mq_timedsend_names[] = {
	"__arm64_sys_mq_timedsend", "__x64_sys_mq_timedsend", "sys_mq_timedsend", NULL,
};
static const char * const vendor_kernel_mq_timedreceive_names[] = {
	"__arm64_sys_mq_timedreceive", "__x64_sys_mq_timedreceive", "sys_mq_timedreceive", NULL,
};
static const char * const vendor_kernel_mq_notify_names[] = {
	"__arm64_sys_mq_notify", "__x64_sys_mq_notify", "sys_mq_notify", NULL,
};
static const char * const vendor_kernel_mq_getsetattr_names[] = {
	"__arm64_sys_mq_getsetattr", "__x64_sys_mq_getsetattr", "sys_mq_getsetattr", NULL,
};
static const char * const vendor_kernel_mount_names[] = {
	"__arm64_sys_mount", "__x64_sys_mount", "sys_mount", NULL,
};
#endif
#ifdef CONFIG_SYSVIPC
static const char * const vendor_kernel_msgget_names[] = {
	"__arm64_sys_msgget", "__x64_sys_msgget", "sys_msgget", NULL,
};
static const char * const vendor_kernel_msgctl_names[] = {
	"__arm64_sys_msgctl", "__x64_sys_msgctl", "sys_msgctl", NULL,
};
static const char * const vendor_kernel_msgsnd_names[] = {
	"__arm64_sys_msgsnd", "__x64_sys_msgsnd", "sys_msgsnd", NULL,
};
static const char * const vendor_kernel_msgrcv_names[] = {
	"__arm64_sys_msgrcv", "__x64_sys_msgrcv", "sys_msgrcv", NULL,
};
static const char * const vendor_kernel_semget_names[] = {
	"__arm64_sys_semget", "__x64_sys_semget", "sys_semget", NULL,
};
static const char * const vendor_kernel_semctl_names[] = {
	"__arm64_sys_semctl", "__x64_sys_semctl", "sys_semctl", NULL,
};
static const char * const vendor_kernel_semop_names[] = {
	"__arm64_sys_semop", "__x64_sys_semop", "sys_semop", NULL,
};
static const char * const vendor_kernel_semtimedop_names[] = {
	"__arm64_sys_semtimedop", "__x64_sys_semtimedop", "sys_semtimedop", NULL,
};
static const char * const vendor_kernel_shmget_names[] = {
	"__arm64_sys_shmget", "__x64_sys_shmget", "sys_shmget", NULL,
};
static const char * const vendor_kernel_shmctl_names[] = {
	"__arm64_sys_shmctl", "__x64_sys_shmctl", "sys_shmctl", NULL,
};
static const char * const vendor_kernel_shmat_names[] = {
	"__arm64_sys_shmat", "__x64_sys_shmat", "sys_shmat", NULL,
};
static const char * const vendor_kernel_shmdt_names[] = {
	"__arm64_sys_shmdt", "__x64_sys_shmdt", "sys_shmdt", NULL,
};
#endif

#if defined(CONFIG_ARM64)
static unsigned long vns_sys_arg(const struct pt_regs *regs, unsigned int n)
{
	return regs->regs[n];
}

static void vns_sys_set_arg(struct pt_regs *regs, unsigned int n,
			    unsigned long value)
{
	regs->regs[n] = value;
}
#elif defined(CONFIG_X86_64)
static unsigned long vns_sys_arg(const struct pt_regs *regs, unsigned int n)
{
	switch (n) {
	case 0: return regs->di;
	case 1: return regs->si;
	case 2: return regs->dx;
	case 3: return regs->r10;
	case 4: return regs->r8;
	case 5: return regs->r9;
	default: return 0;
	}
}

static void vns_sys_set_arg(struct pt_regs *regs, unsigned int n,
			    unsigned long value)
{
	switch (n) {
	case 0: regs->di = value; break;
	case 1: regs->si = value; break;
	case 2: regs->dx = value; break;
	case 3: regs->r10 = value; break;
	case 4: regs->r8 = value; break;
	case 5: regs->r9 = value; break;
	}
}
#else
#error "vendor_kernel: unsupported architecture"
#endif

static long vendor_kernel_hook_unshare(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long flags = vns_sys_arg(regs, 0);
	unsigned long vns_flags = flags & VNS_CLONE_FLAGS;
	unsigned long native_flags = flags & ~VNS_CLONE_FLAGS;
	struct cred *new_cred = NULL;
	struct nsproxy *new_nsp = NULL;
	long ret = 0;

	if (!vns_flags)
		return real_sys_unshare(regs);

	if (native_flags) {
		vns_sys_set_arg(&regs_copy, 0, native_flags);
		ret = real_sys_unshare(&regs_copy);
		if (ret)
			return ret;
	}

	if (vns_flags & CLONE_NEWUSER) {
		ret = vns_unshare_userns(vns_flags, &new_cred);
		if (ret)
			return ret;
	}

	ret = vns_unshare_nsproxy_namespaces(vns_flags, &new_nsp, new_cred, NULL);
	if (ret) {
		if (new_cred)
			put_cred(new_cred);
		return ret;
	}

	if (new_cred)
		commit_creds(new_cred);
	vns_registry_set_nsproxy(task_tgid_nr(current), new_nsp);
	if (new_nsp)
		vns_put_nsproxy(new_nsp);
	vendor_kernel_registry.stat_unshare++;
	return 0;
}

static long vendor_kernel_hook_setns(const struct pt_regs *regs)
{
	int fd = (int)vns_sys_arg(regs, 0);
	int flags = (int)vns_sys_arg(regs, 1);
	long ret = vns_sys_setns(fd, flags);

	if (!ret)
		vendor_kernel_registry.stat_setns++;
	return ret;
}

static void vendor_kernel_clone_track(long ret, unsigned long vns_flags)
{
	struct nsproxy *new_nsp = NULL;

	if (ret <= 0)
		return;
	if (vns_flags) {
		if (!vns_unshare_nsproxy_namespaces(vns_flags, &new_nsp, NULL, NULL)) {
			vns_registry_set_nsproxy((pid_t)ret, new_nsp);
			if (new_nsp)
				vns_put_nsproxy(new_nsp);
		}
	} else {
		vns_registry_clone(task_tgid_nr(current), (pid_t)ret);
	}
	vendor_kernel_registry.stat_clone++;
}

static long vendor_kernel_hook_clone(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long flags = vns_sys_arg(regs, 0);
	unsigned long vns_flags = flags & VNS_CLONE_FLAGS;
	long ret;

	if (vns_flags)
		vns_sys_set_arg(&regs_copy, 0, flags & ~VNS_CLONE_FLAGS);
	ret = real_sys_clone(vns_flags ? &regs_copy : regs);
	vendor_kernel_clone_track(ret, vns_flags);
	return ret;
}

static long vendor_kernel_hook_clone3(const struct pt_regs *regs)
{
	void __user *uargs = (void __user *)(uintptr_t)vns_sys_arg(regs, 0);
	u64 orig_flags;
	u64 native_flags;
	unsigned long vns_flags;
	long ret;
	bool patched = false;

	if (!uargs || copy_from_user(&orig_flags, uargs, sizeof(orig_flags)))
		return real_sys_clone3(regs);

	vns_flags = (unsigned long)(orig_flags & VNS_CLONE_FLAGS);
	native_flags = orig_flags & ~((u64)VNS_CLONE_FLAGS);
	if (vns_flags) {
		if (copy_to_user(uargs, &native_flags, sizeof(native_flags)))
			return -EFAULT;
		patched = true;
	}

	ret = real_sys_clone3(regs);
	if (patched && copy_to_user(uargs, &orig_flags, sizeof(orig_flags)))
		return -EFAULT;
	vendor_kernel_clone_track(ret, vns_flags);
	return ret;
}

static long vendor_kernel_hook_fork(const struct pt_regs *regs)
{
	long ret = real_sys_fork(regs);
	vendor_kernel_clone_track(ret, 0);
	return ret;
}

static long vendor_kernel_hook_vfork(const struct pt_regs *regs)
{
	long ret = real_sys_vfork(regs);
	vendor_kernel_clone_track(ret, 0);
	return ret;
}

#ifdef CONFIG_POSIX_MQUEUE
#define VNS_MOUNT_TYPE_ARG 2
#define VNS_MOUNT_FSTYPE_MAX 32

typedef unsigned long (*vns_vm_mmap_fn)(struct file *file, unsigned long addr,
					unsigned long len, unsigned long prot,
					unsigned long flag, unsigned long offset);
typedef int (*vns_vm_munmap_fn)(unsigned long start, size_t len);

static long vendor_kernel_mount_with_type(const struct pt_regs *regs,
					  const char *type)
{
	struct pt_regs kregs;
	unsigned long scratch;
	long ret;
	static vns_vm_mmap_fn vm_mmap_fn;
	static vns_vm_munmap_fn vm_munmap_fn;

	if (!vm_mmap_fn)
		vm_mmap_fn = (vns_vm_mmap_fn)shadow_hook_resolve("vm_mmap");
	if (!vm_munmap_fn)
		vm_munmap_fn = (vns_vm_munmap_fn)shadow_hook_resolve("vm_munmap");
	if (!vm_mmap_fn || !vm_munmap_fn)
		return -ENOSYS;

	memcpy(&kregs, regs, sizeof(kregs));
	scratch = vm_mmap_fn(NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (IS_ERR_VALUE(scratch))
		return (long)scratch;
	if (copy_to_user((void __user *)scratch, type, strlen(type) + 1)) {
		vm_munmap_fn(scratch, PAGE_SIZE);
		return -EFAULT;
	}
	vns_sys_set_arg(&kregs, VNS_MOUNT_TYPE_ARG, scratch);
	ret = real_sys_mount(&kregs);
	vm_munmap_fn(scratch, PAGE_SIZE);
	return ret;
}

static long vendor_kernel_hook_mount(const struct pt_regs *regs)
{
	const char __user *utype =
		(const char __user *)vns_sys_arg(regs, VNS_MOUNT_TYPE_ARG);
	char type[VNS_MOUNT_FSTYPE_MAX];
	long copied;

	if (!utype)
		return real_sys_mount(regs);
	copied = strncpy_from_user(type, utype, sizeof(type));
	if (copied < 0 || copied >= sizeof(type))
		return real_sys_mount(regs);
	type[copied] = '\0';
	if (strcmp(type, "mqueue"))
		return real_sys_mount(regs);
	return vendor_kernel_mount_with_type(regs, "vendor_kernel_mqueue");
}

static long vendor_kernel_hook_mq_open(const struct pt_regs *regs)
{
	return vns_mq_open((const char __user *)vns_sys_arg(regs, 0),
			   (int)vns_sys_arg(regs, 1),
			   (umode_t)vns_sys_arg(regs, 2),
			   (struct mq_attr __user *)vns_sys_arg(regs, 3));
}

static long vendor_kernel_hook_mq_unlink(const struct pt_regs *regs)
{
	return vns_mq_unlink((const char __user *)vns_sys_arg(regs, 0));
}

static long vendor_kernel_hook_mq_timedsend(const struct pt_regs *regs)
{
	return vns_mq_timedsend((mqd_t)vns_sys_arg(regs, 0),
				  (const char __user *)vns_sys_arg(regs, 1),
				  (size_t)vns_sys_arg(regs, 2),
				  (unsigned int)vns_sys_arg(regs, 3),
				  (const struct __kernel_timespec __user *)vns_sys_arg(regs, 4));
}

static long vendor_kernel_hook_mq_timedreceive(const struct pt_regs *regs)
{
	return vns_mq_timedreceive((mqd_t)vns_sys_arg(regs, 0),
				     (char __user *)vns_sys_arg(regs, 1),
				     (size_t)vns_sys_arg(regs, 2),
				     (unsigned int __user *)vns_sys_arg(regs, 3),
				     (const struct __kernel_timespec __user *)vns_sys_arg(regs, 4));
}

static long vendor_kernel_hook_mq_notify(const struct pt_regs *regs)
{
	return vns_mq_notify((mqd_t)vns_sys_arg(regs, 0),
			     (const struct sigevent __user *)vns_sys_arg(regs, 1));
}

static long vendor_kernel_hook_mq_getsetattr(const struct pt_regs *regs)
{
	return vns_mq_getsetattr((mqd_t)vns_sys_arg(regs, 0),
				 (const struct mq_attr __user *)vns_sys_arg(regs, 1),
				 (struct mq_attr __user *)vns_sys_arg(regs, 2));
}
#endif

#ifdef CONFIG_SYSVIPC
static long vendor_kernel_hook_msgget(const struct pt_regs *regs)
{
	return vns_ksys_msgget((key_t)vns_sys_arg(regs, 0),
			      (int)vns_sys_arg(regs, 1));
}

static long vendor_kernel_hook_msgctl(const struct pt_regs *regs)
{
	return vns_msgctl((int)vns_sys_arg(regs, 0), (int)vns_sys_arg(regs, 1),
			  (struct msqid_ds __user *)vns_sys_arg(regs, 2));
}

static long vendor_kernel_hook_msgsnd(const struct pt_regs *regs)
{
	return vns_ksys_msgsnd((int)vns_sys_arg(regs, 0),
			      (struct msgbuf __user *)vns_sys_arg(regs, 1),
			      (size_t)vns_sys_arg(regs, 2),
			      (int)vns_sys_arg(regs, 3));
}

static long vendor_kernel_hook_msgrcv(const struct pt_regs *regs)
{
	return vns_ksys_msgrcv((int)vns_sys_arg(regs, 0),
			      (struct msgbuf __user *)vns_sys_arg(regs, 1),
			      (size_t)vns_sys_arg(regs, 2),
			      (long)vns_sys_arg(regs, 3),
			      (int)vns_sys_arg(regs, 4));
}

static long vendor_kernel_hook_semget(const struct pt_regs *regs)
{
	return vns_ksys_semget((key_t)vns_sys_arg(regs, 0),
			      (int)vns_sys_arg(regs, 1),
			      (int)vns_sys_arg(regs, 2));
}

static long vendor_kernel_hook_semctl(const struct pt_regs *regs)
{
	return vns_semctl((int)vns_sys_arg(regs, 0), (int)vns_sys_arg(regs, 1),
			  (int)vns_sys_arg(regs, 2), vns_sys_arg(regs, 3));
}

static long vendor_kernel_hook_semop(const struct pt_regs *regs)
{
	return vns_ksys_semtimedop((int)vns_sys_arg(regs, 0),
				  (struct sembuf __user *)vns_sys_arg(regs, 1),
				  (unsigned int)vns_sys_arg(regs, 2), NULL);
}

static long vendor_kernel_hook_semtimedop(const struct pt_regs *regs)
{
	return vns_ksys_semtimedop((int)vns_sys_arg(regs, 0),
				  (struct sembuf __user *)vns_sys_arg(regs, 1),
				  (unsigned int)vns_sys_arg(regs, 2),
				  (const struct __kernel_timespec __user *)vns_sys_arg(regs, 3));
}

static long vendor_kernel_hook_shmget(const struct pt_regs *regs)
{
	return vns_ksys_shmget((key_t)vns_sys_arg(regs, 0),
			      (size_t)vns_sys_arg(regs, 1),
			      (int)vns_sys_arg(regs, 2));
}

static long vendor_kernel_hook_shmctl(const struct pt_regs *regs)
{
	return vns_shmctl((int)vns_sys_arg(regs, 0), (int)vns_sys_arg(regs, 1),
			  (struct shmid_ds __user *)vns_sys_arg(regs, 2));
}

static long vendor_kernel_hook_shmat(const struct pt_regs *regs)
{
	return vns_shmat((int)vns_sys_arg(regs, 0),
			 (char __user *)vns_sys_arg(regs, 1),
			 (int)vns_sys_arg(regs, 2));
}

static long vendor_kernel_hook_shmdt(const struct pt_regs *regs)
{
	return vns_ksys_shmdt((char __user *)vns_sys_arg(regs, 0));
}
#endif

static struct shadow_hook vendor_kernel_unshare_hook =
	SHADOW_HOOK(vendor_kernel_unshare_names, vendor_kernel_hook_unshare, &real_sys_unshare);
static struct shadow_hook vendor_kernel_setns_hook =
	SHADOW_HOOK(vendor_kernel_setns_names, vendor_kernel_hook_setns, &real_sys_setns);
static struct shadow_hook vendor_kernel_clone_hook =
	SHADOW_HOOK(vendor_kernel_clone_names, vendor_kernel_hook_clone, &real_sys_clone);
static struct shadow_hook vendor_kernel_clone3_hook =
	SHADOW_HOOK(vendor_kernel_clone3_names, vendor_kernel_hook_clone3, &real_sys_clone3);
static struct shadow_hook vendor_kernel_fork_hook =
	SHADOW_HOOK(vendor_kernel_fork_names, vendor_kernel_hook_fork, &real_sys_fork);
static struct shadow_hook vendor_kernel_vfork_hook =
	SHADOW_HOOK(vendor_kernel_vfork_names, vendor_kernel_hook_vfork, &real_sys_vfork);
#ifdef CONFIG_POSIX_MQUEUE
static struct shadow_hook vendor_kernel_mq_open_hook =
	SHADOW_HOOK(vendor_kernel_mq_open_names, vendor_kernel_hook_mq_open, &real_sys_mq_open);
static struct shadow_hook vendor_kernel_mq_unlink_hook =
	SHADOW_HOOK(vendor_kernel_mq_unlink_names, vendor_kernel_hook_mq_unlink, &real_sys_mq_unlink);
static struct shadow_hook vendor_kernel_mq_timedsend_hook =
	SHADOW_HOOK(vendor_kernel_mq_timedsend_names, vendor_kernel_hook_mq_timedsend, &real_sys_mq_timedsend);
static struct shadow_hook vendor_kernel_mq_timedreceive_hook =
	SHADOW_HOOK(vendor_kernel_mq_timedreceive_names, vendor_kernel_hook_mq_timedreceive, &real_sys_mq_timedreceive);
static struct shadow_hook vendor_kernel_mq_notify_hook =
	SHADOW_HOOK(vendor_kernel_mq_notify_names, vendor_kernel_hook_mq_notify, &real_sys_mq_notify);
static struct shadow_hook vendor_kernel_mq_getsetattr_hook =
	SHADOW_HOOK(vendor_kernel_mq_getsetattr_names, vendor_kernel_hook_mq_getsetattr, &real_sys_mq_getsetattr);
static struct shadow_hook vendor_kernel_mount_hook =
	SHADOW_HOOK(vendor_kernel_mount_names, vendor_kernel_hook_mount, &real_sys_mount);
#endif
#ifdef CONFIG_SYSVIPC
static struct shadow_hook vendor_kernel_msgget_hook =
	SHADOW_HOOK(vendor_kernel_msgget_names, vendor_kernel_hook_msgget, &real_sys_msgget);
static struct shadow_hook vendor_kernel_msgctl_hook =
	SHADOW_HOOK(vendor_kernel_msgctl_names, vendor_kernel_hook_msgctl, &real_sys_msgctl);
static struct shadow_hook vendor_kernel_msgsnd_hook =
	SHADOW_HOOK(vendor_kernel_msgsnd_names, vendor_kernel_hook_msgsnd, &real_sys_msgsnd);
static struct shadow_hook vendor_kernel_msgrcv_hook =
	SHADOW_HOOK(vendor_kernel_msgrcv_names, vendor_kernel_hook_msgrcv, &real_sys_msgrcv);
static struct shadow_hook vendor_kernel_semget_hook =
	SHADOW_HOOK(vendor_kernel_semget_names, vendor_kernel_hook_semget, &real_sys_semget);
static struct shadow_hook vendor_kernel_semctl_hook =
	SHADOW_HOOK(vendor_kernel_semctl_names, vendor_kernel_hook_semctl, &real_sys_semctl);
static struct shadow_hook vendor_kernel_semop_hook =
	SHADOW_HOOK(vendor_kernel_semop_names, vendor_kernel_hook_semop, &real_sys_semop);
static struct shadow_hook vendor_kernel_semtimedop_hook =
	SHADOW_HOOK(vendor_kernel_semtimedop_names, vendor_kernel_hook_semtimedop, &real_sys_semtimedop);
static struct shadow_hook vendor_kernel_shmget_hook =
	SHADOW_HOOK(vendor_kernel_shmget_names, vendor_kernel_hook_shmget, &real_sys_shmget);
static struct shadow_hook vendor_kernel_shmctl_hook =
	SHADOW_HOOK(vendor_kernel_shmctl_names, vendor_kernel_hook_shmctl, &real_sys_shmctl);
static struct shadow_hook vendor_kernel_shmat_hook =
	SHADOW_HOOK(vendor_kernel_shmat_names, vendor_kernel_hook_shmat, &real_sys_shmat);
static struct shadow_hook vendor_kernel_shmdt_hook =
	SHADOW_HOOK(vendor_kernel_shmdt_names, vendor_kernel_hook_shmdt, &real_sys_shmdt);
#endif

struct shadow_hook *vendor_kernel_core_hooks[] = {
	&vendor_kernel_unshare_hook,
	&vendor_kernel_setns_hook,
	&vendor_kernel_clone_hook,
	&vendor_kernel_clone3_hook,
	&vendor_kernel_fork_hook,
	&vendor_kernel_vfork_hook,
#ifdef CONFIG_POSIX_MQUEUE
	&vendor_kernel_mq_open_hook,
	&vendor_kernel_mq_unlink_hook,
	&vendor_kernel_mq_timedsend_hook,
	&vendor_kernel_mq_timedreceive_hook,
	&vendor_kernel_mq_notify_hook,
	&vendor_kernel_mq_getsetattr_hook,
	&vendor_kernel_mount_hook,
#endif
#ifdef CONFIG_SYSVIPC
	&vendor_kernel_msgget_hook,
	&vendor_kernel_msgctl_hook,
	&vendor_kernel_msgsnd_hook,
	&vendor_kernel_msgrcv_hook,
	&vendor_kernel_semget_hook,
	&vendor_kernel_semctl_hook,
	&vendor_kernel_semop_hook,
	&vendor_kernel_semtimedop_hook,
	&vendor_kernel_shmget_hook,
	&vendor_kernel_shmctl_hook,
	&vendor_kernel_shmat_hook,
	&vendor_kernel_shmdt_hook,
#endif
	NULL,
};
