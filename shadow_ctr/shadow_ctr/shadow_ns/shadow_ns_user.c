// SPDX-License-Identifier: GPL-2.0
#include "shadow_ns_internal.h"

static long (*real_sys_getuid)(const struct pt_regs *regs);
static long (*real_sys_geteuid)(const struct pt_regs *regs);
static long (*real_sys_getgid)(const struct pt_regs *regs);
static long (*real_sys_getegid)(const struct pt_regs *regs);
static long (*real_sys_getresuid)(const struct pt_regs *regs);
static long (*real_sys_getresgid)(const struct pt_regs *regs);

static const char * const shadow_ns_getuid_names[] = {
	"__arm64_sys_getuid", "__x64_sys_getuid", "sys_getuid", NULL,
};
static const char * const shadow_ns_geteuid_names[] = {
	"__arm64_sys_geteuid", "__x64_sys_geteuid", "sys_geteuid", NULL,
};
static const char * const shadow_ns_getgid_names[] = {
	"__arm64_sys_getgid", "__x64_sys_getgid", "sys_getgid", NULL,
};
static const char * const shadow_ns_getegid_names[] = {
	"__arm64_sys_getegid", "__x64_sys_getegid", "sys_getegid", NULL,
};
static const char * const shadow_ns_getresuid_names[] = {
	"__arm64_sys_getresuid", "__x64_sys_getresuid", "sys_getresuid", NULL,
};
static const char * const shadow_ns_getresgid_names[] = {
	"__arm64_sys_getresgid", "__x64_sys_getresgid", "sys_getresgid", NULL,
};

struct shadow_userns_priv *shadow_ns_userns_priv_alloc(void)
{
	struct shadow_userns_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	priv->real_uid = current_uid();
	priv->real_gid = current_gid();
	return priv;
}

void shadow_ns_userns_priv_free(struct shadow_userns_priv *priv)
{
	kfree(priv);
}

static struct shadow_userns_priv *shadow_ns_current_userns_priv(void)
{
	struct shadow_ns *ns = shadow_ns_get_current(SHADOW_NS_TYPE_USER);
	struct shadow_userns_priv *priv;

	if (!ns)
		return NULL;
	priv = ns->user;
	shadow_ns_put(ns);
	return priv;
}

static long shadow_ns_hook_getuid(const struct pt_regs *regs)
{
	return shadow_ns_current_userns_priv() ? 0 : real_sys_getuid(regs);
}

static long shadow_ns_hook_geteuid(const struct pt_regs *regs)
{
	return shadow_ns_current_userns_priv() ? 0 : real_sys_geteuid(regs);
}

static long shadow_ns_hook_getgid(const struct pt_regs *regs)
{
	return shadow_ns_current_userns_priv() ? 0 : real_sys_getgid(regs);
}

static long shadow_ns_hook_getegid(const struct pt_regs *regs)
{
	return shadow_ns_current_userns_priv() ? 0 : real_sys_getegid(regs);
}

static long shadow_ns_hook_getresuid(const struct pt_regs *regs)
{
	void __user *ruid = (void __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	void __user *euid = (void __user *)(uintptr_t)shadow_ns_sys_arg1(regs);
	void __user *suid;
	long ret = real_sys_getresuid(regs);
	uid_t zero = 0;

	if (ret || !shadow_ns_current_userns_priv())
		return ret;

#if defined(CONFIG_ARM64)
	suid = (void __user *)(uintptr_t)regs->regs[2];
#else
	suid = (void __user *)(uintptr_t)regs->dx;
#endif
	if (copy_to_user(ruid, &zero, sizeof(zero)) ||
	    copy_to_user(euid, &zero, sizeof(zero)) ||
	    copy_to_user(suid, &zero, sizeof(zero)))
		return -EFAULT;
	return 0;
}

static long shadow_ns_hook_getresgid(const struct pt_regs *regs)
{
	void __user *rgid = (void __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	void __user *egid = (void __user *)(uintptr_t)shadow_ns_sys_arg1(regs);
	void __user *sgid;
	long ret = real_sys_getresgid(regs);
	gid_t zero = 0;

	if (ret || !shadow_ns_current_userns_priv())
		return ret;

#if defined(CONFIG_ARM64)
	sgid = (void __user *)(uintptr_t)regs->regs[2];
#else
	sgid = (void __user *)(uintptr_t)regs->dx;
#endif
	if (copy_to_user(rgid, &zero, sizeof(zero)) ||
	    copy_to_user(egid, &zero, sizeof(zero)) ||
	    copy_to_user(sgid, &zero, sizeof(zero)))
		return -EFAULT;
	return 0;
}

static struct shadow_hook shadow_ns_getuid_hook =
	SHADOW_HOOK(shadow_ns_getuid_names, shadow_ns_hook_getuid, &real_sys_getuid);
static struct shadow_hook shadow_ns_geteuid_hook =
	SHADOW_HOOK(shadow_ns_geteuid_names, shadow_ns_hook_geteuid, &real_sys_geteuid);
static struct shadow_hook shadow_ns_getgid_hook =
	SHADOW_HOOK(shadow_ns_getgid_names, shadow_ns_hook_getgid, &real_sys_getgid);
static struct shadow_hook shadow_ns_getegid_hook =
	SHADOW_HOOK(shadow_ns_getegid_names, shadow_ns_hook_getegid, &real_sys_getegid);
static struct shadow_hook shadow_ns_getresuid_hook =
	SHADOW_HOOK(shadow_ns_getresuid_names, shadow_ns_hook_getresuid,
		    &real_sys_getresuid);
static struct shadow_hook shadow_ns_getresgid_hook =
	SHADOW_HOOK(shadow_ns_getresgid_names, shadow_ns_hook_getresgid,
		    &real_sys_getresgid);

struct shadow_hook *shadow_ns_user_hooks[] = {
	&shadow_ns_getuid_hook,
	&shadow_ns_geteuid_hook,
	&shadow_ns_getgid_hook,
	&shadow_ns_getegid_hook,
	&shadow_ns_getresuid_hook,
	&shadow_ns_getresgid_hook,
	NULL,
};
