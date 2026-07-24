// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_ns_syscalls.c - unconditional syscall interception layer.
 *
 * This is vendor_ns's own hook plumbing (not vendored kernel source); it uses
 * the repository's shared shadow_hook ftrace/kprobe engine (common/shadow_hook.h)
 * exactly as the other lkm4ctr submodules do. Unlike shadow_ns, every hook here
 * is installed unconditionally regardless of the running kernel's CONFIG_*_NS:
 * vendor_ns always tracks namespace membership in its own vendored bookkeeping
 * and always serves UTS/PID/USER results from its vendored namespaces.
 *
 * Each replacement calls through to the genuine syscall (so the host kernel's
 * real namespace machinery still runs underneath) and then layers vendor_ns's
 * vendored view on top -- recording membership on unshare/setns/clone, storing
 * hostnames in the vendored UTS namespace on sethostname/setdomainname, and
 * translating pid/uid results through the vendored pid/user namespaces.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/utsname.h>

#include "../vendor_ns.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

/* ------------------------------------------------------------------ */
/* pt_regs syscall-argument accessors (arch-specific).		    */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ARM64)
unsigned long vns_sys_arg0(const struct pt_regs *regs) { return regs->regs[0]; }
unsigned long vns_sys_arg1(const struct pt_regs *regs) { return regs->regs[1]; }
unsigned long vns_sys_arg2(const struct pt_regs *regs) { return regs->regs[2]; }
unsigned long vns_sys_arg3(const struct pt_regs *regs) { return regs->regs[3]; }
#elif defined(CONFIG_X86_64)
unsigned long vns_sys_arg0(const struct pt_regs *regs) { return regs->di; }
unsigned long vns_sys_arg1(const struct pt_regs *regs) { return regs->si; }
unsigned long vns_sys_arg2(const struct pt_regs *regs) { return regs->dx; }
unsigned long vns_sys_arg3(const struct pt_regs *regs) { return regs->r10; }
#else
unsigned long vns_sys_arg0(const struct pt_regs *regs)
{
	return regs_get_kernel_argument(regs, 0);
}
unsigned long vns_sys_arg1(const struct pt_regs *regs)
{
	return regs_get_kernel_argument(regs, 1);
}
unsigned long vns_sys_arg2(const struct pt_regs *regs)
{
	return regs_get_kernel_argument(regs, 2);
}
unsigned long vns_sys_arg3(const struct pt_regs *regs)
{
	return regs_get_kernel_argument(regs, 3);
}
#endif

/* ------------------------------------------------------------------ */
/* core: unshare / setns / clone / clone3 / fork / vfork		    */
/* ------------------------------------------------------------------ */

static long (*real_sys_unshare)(const struct pt_regs *regs);
static long (*real_sys_setns)(const struct pt_regs *regs);
static long (*real_sys_clone)(const struct pt_regs *regs);
static long (*real_sys_clone3)(const struct pt_regs *regs);
static long (*real_sys_fork)(const struct pt_regs *regs);
static long (*real_sys_vfork)(const struct pt_regs *regs);

static const char * const vns_unshare_names[] = {
	"__arm64_sys_unshare", "__x64_sys_unshare", "sys_unshare", NULL,
};
static const char * const vns_setns_names[] = {
	"__arm64_sys_setns", "__x64_sys_setns", "sys_setns", NULL,
};
static const char * const vns_clone_names[] = {
	"__arm64_sys_clone", "__x64_sys_clone", "sys_clone", NULL,
};
static const char * const vns_clone3_names[] = {
	"__arm64_sys_clone3", "__x64_sys_clone3", "sys_clone3", NULL,
};
static const char * const vns_fork_names[] = {
	"__arm64_sys_fork", "__x64_sys_fork", "sys_fork", NULL,
};
static const char * const vns_vfork_names[] = {
	"__arm64_sys_vfork", "__x64_sys_vfork", "sys_vfork", NULL,
};

static long vns_hook_unshare(const struct pt_regs *regs)
{
	unsigned long flags = vns_sys_arg0(regs);
	long ret = real_sys_unshare(regs);

	if (ret == 0)
		vns_do_unshare(flags);
	return ret;
}

static long vns_hook_setns(const struct pt_regs *regs)
{
	int flags = (int)vns_sys_arg1(regs);
	long ret = real_sys_setns(regs);

	if (ret == 0)
		vns_do_setns_flags((unsigned long)flags);
	return ret;
}

static long vns_hook_clone(const struct pt_regs *regs)
{
	unsigned long flags = vns_sys_arg0(regs);
	long ret = real_sys_clone(regs);

	if (ret > 0)
		vns_track_child_flags((pid_t)ret, flags);
	return ret;
}

static long vns_hook_clone3(const struct pt_regs *regs)
{
	void __user *uargs = (void __user *)(uintptr_t)vns_sys_arg0(regs);
	u64 flags = 0;
	long ret;

	if (uargs && copy_from_user(&flags, uargs, sizeof(flags)))
		flags = 0;

	ret = real_sys_clone3(regs);
	if (ret > 0)
		vns_track_child_flags((pid_t)ret, (unsigned long)flags);
	return ret;
}

static long vns_hook_fork(const struct pt_regs *regs)
{
	long ret = real_sys_fork(regs);

	if (ret > 0)
		vns_track_child((pid_t)ret);
	return ret;
}

static long vns_hook_vfork(const struct pt_regs *regs)
{
	long ret = real_sys_vfork(regs);

	if (ret > 0)
		vns_track_child((pid_t)ret);
	return ret;
}

static struct shadow_hook vns_unshare_hook =
	SHADOW_HOOK(vns_unshare_names, vns_hook_unshare, &real_sys_unshare);
static struct shadow_hook vns_setns_hook =
	SHADOW_HOOK(vns_setns_names, vns_hook_setns, &real_sys_setns);
static struct shadow_hook vns_clone_hook =
	SHADOW_HOOK(vns_clone_names, vns_hook_clone, &real_sys_clone);
static struct shadow_hook vns_clone3_hook =
	SHADOW_HOOK(vns_clone3_names, vns_hook_clone3, &real_sys_clone3);
static struct shadow_hook vns_fork_hook =
	SHADOW_HOOK(vns_fork_names, vns_hook_fork, &real_sys_fork);
static struct shadow_hook vns_vfork_hook =
	SHADOW_HOOK(vns_vfork_names, vns_hook_vfork, &real_sys_vfork);

struct shadow_hook *vendor_ns_core_hooks[] = {
	&vns_unshare_hook,
	&vns_setns_hook,
	&vns_clone_hook,
	&vns_clone3_hook,
	&vns_fork_hook,
	&vns_vfork_hook,
	NULL,
};

/* ------------------------------------------------------------------ */
/* UTS: sethostname / setdomainname / uname			    */
/* ------------------------------------------------------------------ */

static long (*real_sys_sethostname)(const struct pt_regs *regs);
static long (*real_sys_setdomainname)(const struct pt_regs *regs);
static long (*real_sys_newuname)(const struct pt_regs *regs);

static const char * const vns_sethostname_names[] = {
	"__arm64_sys_sethostname", "__x64_sys_sethostname", "sys_sethostname",
	NULL,
};
static const char * const vns_setdomainname_names[] = {
	"__arm64_sys_setdomainname", "__x64_sys_setdomainname",
	"sys_setdomainname", NULL,
};
static const char * const vns_newuname_names[] = {
	"__arm64_sys_newuname", "__x64_sys_newuname", "sys_newuname", NULL,
};

static long vns_store_uts(const struct pt_regs *regs, bool domain)
{
	char __user *uname = (char __user *)(uintptr_t)vns_sys_arg0(regs);
	int len = (int)vns_sys_arg1(regs);
	char kbuf[__NEW_UTS_LEN + 1];

	if (len < 0)
		return -EINVAL;
	if (len > __NEW_UTS_LEN)
		len = __NEW_UTS_LEN;

	memset(kbuf, 0, sizeof(kbuf));
	if (uname && len && copy_from_user(kbuf, uname, len))
		return -EFAULT;

	vns_uts_set(kbuf, len, domain);
	return 0;
}

static long vns_hook_sethostname(const struct pt_regs *regs)
{
	long ret = real_sys_sethostname(regs);

	if (ret == 0)
		vns_store_uts(regs, false);
	return ret;
}

static long vns_hook_setdomainname(const struct pt_regs *regs)
{
	long ret = real_sys_setdomainname(regs);

	if (ret == 0)
		vns_store_uts(regs, true);
	return ret;
}

/*
 * struct new_utsname is six back-to-back __NEW_UTS_LEN+1 (65) byte fields:
 * sysname, nodename, release, version, machine, domainname. After the genuine
 * uname(2) fills the user buffer we overlay the vendored nodename/domainname if
 * the calling namespace set them.
 */
#define VNS_UTS_FIELD_SZ	(__NEW_UTS_LEN + 1)
#define VNS_UTS_OFF_NODENAME	(1 * VNS_UTS_FIELD_SZ)
#define VNS_UTS_OFF_DOMAIN	(5 * VNS_UTS_FIELD_SZ)

static long vns_hook_newuname(const struct pt_regs *regs)
{
	char __user *ubuf = (char __user *)(uintptr_t)vns_sys_arg0(regs);
	long ret = real_sys_newuname(regs);
	char field[VNS_UTS_FIELD_SZ];

	if (ret != 0 || !ubuf)
		return ret;

	if (vns_uts_get(field, sizeof(field), false) == 0) {
		if (copy_to_user(ubuf + VNS_UTS_OFF_NODENAME, field,
				 sizeof(field)))
			LKM4CTR_WARN(VENDOR_NS_TAG,
				     "uname: failed to overlay vendored nodename");
	}
	if (vns_uts_get(field, sizeof(field), true) == 0) {
		if (copy_to_user(ubuf + VNS_UTS_OFF_DOMAIN, field,
				 sizeof(field)))
			LKM4CTR_WARN(VENDOR_NS_TAG,
				     "uname: failed to overlay vendored domainname");
	}
	return ret;
}

static struct shadow_hook vns_sethostname_hook =
	SHADOW_HOOK(vns_sethostname_names, vns_hook_sethostname,
		    &real_sys_sethostname);
static struct shadow_hook vns_setdomainname_hook =
	SHADOW_HOOK(vns_setdomainname_names, vns_hook_setdomainname,
		    &real_sys_setdomainname);
static struct shadow_hook vns_newuname_hook =
	SHADOW_HOOK(vns_newuname_names, vns_hook_newuname, &real_sys_newuname);

struct shadow_hook *vendor_ns_uts_hooks[] = {
	&vns_sethostname_hook,
	&vns_setdomainname_hook,
	&vns_newuname_hook,
	NULL,
};

/* ------------------------------------------------------------------ */
/* PID: getpid / getppid / gettid / getpgid / getsid		    */
/* ------------------------------------------------------------------ */

static long (*real_sys_getpid)(const struct pt_regs *regs);
static long (*real_sys_getppid)(const struct pt_regs *regs);
static long (*real_sys_gettid)(const struct pt_regs *regs);
static long (*real_sys_getpgid)(const struct pt_regs *regs);
static long (*real_sys_getsid)(const struct pt_regs *regs);

static const char * const vns_getpid_names[] = {
	"__arm64_sys_getpid", "__x64_sys_getpid", "sys_getpid", NULL,
};
static const char * const vns_getppid_names[] = {
	"__arm64_sys_getppid", "__x64_sys_getppid", "sys_getppid", NULL,
};
static const char * const vns_gettid_names[] = {
	"__arm64_sys_gettid", "__x64_sys_gettid", "sys_gettid", NULL,
};
static const char * const vns_getpgid_names[] = {
	"__arm64_sys_getpgid", "__x64_sys_getpgid", "sys_getpgid", NULL,
};
static const char * const vns_getsid_names[] = {
	"__arm64_sys_getsid", "__x64_sys_getsid", "sys_getsid", NULL,
};

static long vns_hook_getpid(const struct pt_regs *regs)
{
	long ret = real_sys_getpid(regs);

	if (ret > 0)
		return vns_pid_translate((pid_t)ret);
	return ret;
}

static long vns_hook_getppid(const struct pt_regs *regs)
{
	long ret = real_sys_getppid(regs);

	if (ret > 0)
		return vns_pid_translate((pid_t)ret);
	return ret;
}

static long vns_hook_gettid(const struct pt_regs *regs)
{
	long ret = real_sys_gettid(regs);

	if (ret > 0)
		return vns_pid_translate((pid_t)ret);
	return ret;
}

static long vns_hook_getpgid(const struct pt_regs *regs)
{
	long ret = real_sys_getpgid(regs);

	if (ret > 0)
		return vns_pid_translate((pid_t)ret);
	return ret;
}

static long vns_hook_getsid(const struct pt_regs *regs)
{
	long ret = real_sys_getsid(regs);

	if (ret > 0)
		return vns_pid_translate((pid_t)ret);
	return ret;
}

static struct shadow_hook vns_getpid_hook =
	SHADOW_HOOK(vns_getpid_names, vns_hook_getpid, &real_sys_getpid);
static struct shadow_hook vns_getppid_hook =
	SHADOW_HOOK(vns_getppid_names, vns_hook_getppid, &real_sys_getppid);
static struct shadow_hook vns_gettid_hook =
	SHADOW_HOOK(vns_gettid_names, vns_hook_gettid, &real_sys_gettid);
static struct shadow_hook vns_getpgid_hook =
	SHADOW_HOOK(vns_getpgid_names, vns_hook_getpgid, &real_sys_getpgid);
static struct shadow_hook vns_getsid_hook =
	SHADOW_HOOK(vns_getsid_names, vns_hook_getsid, &real_sys_getsid);

struct shadow_hook *vendor_ns_pid_hooks[] = {
	&vns_getpid_hook,
	&vns_getppid_hook,
	&vns_gettid_hook,
	&vns_getpgid_hook,
	&vns_getsid_hook,
	NULL,
};

/* ------------------------------------------------------------------ */
/* USER: getuid / geteuid / getgid / getegid			    */
/* ------------------------------------------------------------------ */

static long (*real_sys_getuid)(const struct pt_regs *regs);
static long (*real_sys_geteuid)(const struct pt_regs *regs);
static long (*real_sys_getgid)(const struct pt_regs *regs);
static long (*real_sys_getegid)(const struct pt_regs *regs);

static const char * const vns_getuid_names[] = {
	"__arm64_sys_getuid", "__x64_sys_getuid", "sys_getuid", NULL,
};
static const char * const vns_geteuid_names[] = {
	"__arm64_sys_geteuid", "__x64_sys_geteuid", "sys_geteuid", NULL,
};
static const char * const vns_getgid_names[] = {
	"__arm64_sys_getgid", "__x64_sys_getgid", "sys_getgid", NULL,
};
static const char * const vns_getegid_names[] = {
	"__arm64_sys_getegid", "__x64_sys_getegid", "sys_getegid", NULL,
};

static long vns_hook_getuid(const struct pt_regs *regs)
{
	long ret = real_sys_getuid(regs);

	if (ret >= 0)
		return vns_uid_translate((uid_t)ret);
	return ret;
}

static long vns_hook_geteuid(const struct pt_regs *regs)
{
	long ret = real_sys_geteuid(regs);

	if (ret >= 0)
		return vns_uid_translate((uid_t)ret);
	return ret;
}

static long vns_hook_getgid(const struct pt_regs *regs)
{
	long ret = real_sys_getgid(regs);

	if (ret >= 0)
		return vns_gid_translate((gid_t)ret);
	return ret;
}

static long vns_hook_getegid(const struct pt_regs *regs)
{
	long ret = real_sys_getegid(regs);

	if (ret >= 0)
		return vns_gid_translate((gid_t)ret);
	return ret;
}

static struct shadow_hook vns_getuid_hook =
	SHADOW_HOOK(vns_getuid_names, vns_hook_getuid, &real_sys_getuid);
static struct shadow_hook vns_geteuid_hook =
	SHADOW_HOOK(vns_geteuid_names, vns_hook_geteuid, &real_sys_geteuid);
static struct shadow_hook vns_getgid_hook =
	SHADOW_HOOK(vns_getgid_names, vns_hook_getgid, &real_sys_getgid);
static struct shadow_hook vns_getegid_hook =
	SHADOW_HOOK(vns_getegid_names, vns_hook_getegid, &real_sys_getegid);

struct shadow_hook *vendor_ns_user_hooks[] = {
	&vns_getuid_hook,
	&vns_geteuid_hook,
	&vns_getgid_hook,
	&vns_getegid_hook,
	NULL,
};
