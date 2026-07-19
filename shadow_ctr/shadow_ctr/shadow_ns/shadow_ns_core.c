// SPDX-License-Identifier: GPL-2.0
#include "shadow_ns_internal.h"

static long (*real_sys_unshare)(const struct pt_regs *regs);
static long (*real_sys_setns)(const struct pt_regs *regs);
static long (*real_sys_clone)(const struct pt_regs *regs);
static long (*real_sys_clone3)(const struct pt_regs *regs);
static long (*real_sys_fork)(const struct pt_regs *regs);
static long (*real_sys_vfork)(const struct pt_regs *regs);

static const char * const shadow_ns_unshare_names[] = {
	"__arm64_sys_unshare", "__x64_sys_unshare", "sys_unshare", NULL,
};
static const char * const shadow_ns_setns_names[] = {
	"__arm64_sys_setns", "__x64_sys_setns", "sys_setns", NULL,
};
static const char * const shadow_ns_clone_names[] = {
	"__arm64_sys_clone", "__x64_sys_clone", "sys_clone", NULL,
};
static const char * const shadow_ns_clone3_names[] = {
	"__arm64_sys_clone3", "__x64_sys_clone3", "sys_clone3", NULL,
};
static const char * const shadow_ns_fork_names[] = {
	"__arm64_sys_fork", "__x64_sys_fork", "sys_fork", NULL,
};
static const char * const shadow_ns_vfork_names[] = {
	"__arm64_sys_vfork", "__x64_sys_vfork", "sys_vfork", NULL,
};

static long shadow_ns_hook_unshare(const struct pt_regs *regs)
{
	struct shadow_task_group *tg;
	struct pt_regs regs_copy;
	unsigned long flags = shadow_ns_sys_arg0(regs);
	unsigned long shadow_flags = flags & shadow_ns_clone_flags;
	unsigned long native_flags = flags & ~shadow_ns_clone_flags;
	long ret = 0;

	if (!shadow_flags)
		return real_sys_unshare(regs);
	if (shadow_ns_requires_admin(shadow_flags) && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (native_flags) {
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
	unsigned long shadow_flags = flags & shadow_ns_clone_flags;
	long ret;

	if (shadow_flags && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (shadow_flags)
		shadow_ns_sys_set_arg0(&regs_copy, flags & ~shadow_ns_clone_flags);
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

	shadow_flags = (unsigned long)(orig_flags & shadow_ns_clone_flags);
	if (shadow_flags && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	native_flags = orig_flags & ~((u64)shadow_ns_clone_flags);
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

struct shadow_hook *shadow_ns_core_hooks[] = {
	&shadow_ns_unshare_hook,
	&shadow_ns_setns_hook,
	&shadow_ns_clone_hook,
	&shadow_ns_clone3_hook,
	&shadow_ns_fork_hook,
	&shadow_ns_vfork_hook,
	NULL,
};
