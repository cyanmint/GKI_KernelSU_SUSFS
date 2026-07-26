// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_procfs.c - fabricate /proc/<pid>/ns/ipc's readlink(2) target
 * on kernels genuinely missing CONFIG_IPC_NS.
 *
 * This is NEW code (not vendored from kernel-common).
 *
 * Why this exists
 * ----------------
 * vendor_kernel_hook_unshare() (glue/vendor_kernel_syscalls.c) already does
 * real work for CLONE_NEWIPC: it builds a genuine vendored `struct
 * ipc_namespace` (ipc/namespace.c's create_ipc_ns(), ported near-verbatim
 * from kernel-common) and installs it on the calling task via
 * vns_switch_task_namespaces(), exactly like the real unshare(2) would.
 * SysV IPC and POSIX mqueue syscalls observe this correctly through
 * vns_current_ipc_ns() (glue/vendor_kernel_ipc_syscalls.c).
 *
 * However, nothing about that makes /proc/<pid>/ns/ipc itself resolvable.
 * fs/proc/namespaces.c's `ns_entries[]` table only includes
 * `&ipcns_operations` `#ifdef CONFIG_IPC_NS` -- a compile-time decision
 * baked into vmlinux. On a kernel genuinely built with CONFIG_IPC_NS=n (the
 * whole reason vendor_kernel's IPC namespace exists), that procfs directory
 * entry does not exist at all, regardless of what task->nsproxy->ipc_ns
 * actually points to: readlink(2) on that path fails with plain -ENOENT.
 *
 * That breaks two independent things:
 *   - runc/containerd's own namespace-support probe, which stats
 *     /proc/<pid>/ns/{ipc,pid,user,uts,...} as part of a single combined
 *     check before issuing unshare()/clone3() (see shadow_ns_procfs.c's
 *     near-identical rationale for pid/pid_for_children/user).
 *   - any external tool (including lkm4ctr_checker) that verifies real
 *     namespace isolation by diffing /proc/self/ns/ipc's readlink(2) target
 *     before and after unshare(CLONE_NEWIPC): with no fabrication, the
 *     "after" readlink still fails with -ENOENT exactly like the "before"
 *     one, so the diff-based probe cannot observe vendor_kernel's real,
 *     already-working ipc_namespace isolation and reports a false STUB.
 *
 * This file closes that observability gap: hook readlink(2)/readlinkat(2),
 * let the real syscall run first, and only when it fails with -ENOENT for a
 * path unambiguously naming ".../ns/ipc" under a procfs-rooted pid
 * directory (".../<pid|self|thread-self>/ns/ipc", or a bare "ns/ipc"
 * resolved relative to a dfd whose superblock is procfs) do we fabricate the
 * "ipc:[<ino>]" text real readlink(2) would have produced, mirroring
 * fs/nsfs.c's ns_get_name() format exactly. Any other -ENOENT (including
 * every other ns/ entry) passes through untouched.
 *
 * The fabricated inode number comes straight from vns_task_ipc_ns(task)'s
 * `ns.inum` (allocated by vns_alloc_inum() -- see ipc/namespace.c's
 * create_ipc_ns() and vns_ipc_default_init() below), so it is self
 * consistent with the real vendored namespace object the SysV/mqueue
 * syscalls already operate against: a task that never unshare(CLONE_NEWIPC)
 * reads back vendor_kernel's own default namespace's inum, and a task that
 * did reads back a distinct one, exactly matching real kernel semantics.
 *
 * This mirrors shadow_ns_procfs.c's readlink(2) fabrication for
 * .../ns/{pid,pid_for_children,user} almost exactly, but is scoped
 * separately here because vendor_kernel tracks its per-task effective
 * ipc_namespace through the real (vendored) task->nsproxy->ipc_ns pointer
 * rather than a bespoke bookkeeping registry, and only IPC gets a
 * fabricated entry: NET/MNT/CGROUP namespaces remain deliberately
 * bookkeeping-only (see shadow_ns/README.md), so fabricating their /proc/ns
 * entries here would misreport nonexistent isolation as real.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/magic.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <asm/ptrace.h>

#include "../vendor_kernel.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_compat.h"

#define VNS_PROC_NS_IPC_NAME	"ipc"

#if defined(CONFIG_ARM64)
static unsigned long vns_procfs_arg0(const struct pt_regs *regs) { return regs->regs[0]; }
static unsigned long vns_procfs_arg1(const struct pt_regs *regs) { return regs->regs[1]; }
static unsigned long vns_procfs_arg2(const struct pt_regs *regs) { return regs->regs[2]; }
static unsigned long vns_procfs_arg3(const struct pt_regs *regs) { return regs->regs[3]; }
#elif defined(CONFIG_X86_64)
static unsigned long vns_procfs_arg0(const struct pt_regs *regs) { return regs->di; }
static unsigned long vns_procfs_arg1(const struct pt_regs *regs) { return regs->si; }
static unsigned long vns_procfs_arg2(const struct pt_regs *regs) { return regs->dx; }
static unsigned long vns_procfs_arg3(const struct pt_regs *regs) { return regs->r10; }
#else
#error "vendor_kernel: unsupported architecture"
#endif

/* Bare "self"/"thread-self"/numeric-pid path component, matching
 * shadow_ns_component_is_pid_dir()'s definition of a valid procfs pid dir. */
static bool vns_component_is_pid_dir(const char *s)
{
	if (!*s)
		return false;
	if (!strcmp(s, "self") || !strcmp(s, "thread-self"))
		return true;
	for (; *s; s++) {
		if (!isdigit((unsigned char)*s))
			return false;
	}
	return true;
}

/*
 * A bare "ns/ipc" path component with no directory prefix only makes sense
 * relative to a dfd that is itself rooted somewhere inside a procfs mount
 * (a detached fsopen("proc")+fsmount() dirfd, or an already-open
 * /proc/<pid> directory fd).
 */
static bool vns_dfd_is_procfs(int dfd)
{
	struct fd f;
	bool ret;

	if (dfd == AT_FDCWD)
		return false;

	f = fdget(dfd);
	if (fd_empty(f))
		return false;
	ret = fd_file(f)->f_path.dentry->d_sb->s_magic == PROC_SUPER_MAGIC;
	fdput(f);
	return ret;
}

/*
 * vns_resolve_ns_ipc_pid() - resolve the "self"/"thread-self"/numeric path
 * component immediately preceding "/ns/ipc" to the real (host) pid it
 * names. Numeric components are taken to already be real pids: unlike
 * shadow_ns, vendor_kernel's vendored PID namespace still installs the real
 * task->nsproxy, so no separate vpid<->rpid translation table is needed
 * here.
 */
static pid_t vns_resolve_ns_ipc_pid(const char *comp)
{
	long val;

	if (!strcmp(comp, "self"))
		return task_tgid_nr(current);
	if (!strcmp(comp, "thread-self"))
		return task_pid_nr(current);
	if (kstrtol(comp, 10, &val) || val <= 0 || val > INT_MAX)
		return 0;
	return (pid_t)val;
}

/*
 * vns_path_is_ns_ipc() - does @upath (relative to @dfd) name
 * ".../<piddir>/ns/ipc"? If so, resolves the owning task's real pid into
 * *rpid and returns true. Returns false otherwise (including on any parse
 * failure) -- callers must fall back to the real syscall unchanged.
 */
static bool vns_path_is_ns_ipc(int dfd, const char __user *upath, pid_t *rpid)
{
	char buf[192];
	char *base, *slash1, *piddir;
	long n;

	if (!upath)
		return false;

	n = strncpy_from_user(buf, upath, sizeof(buf));
	if (n <= 0 || n >= sizeof(buf))
		return false;

	if (buf[0] == '/') {
		if (strncmp(buf, "/proc/", 6))
			return false;
		base = buf + 6;
	} else {
		if (dfd == AT_FDCWD || !vns_dfd_is_procfs(dfd))
			return false;
		base = buf;
	}

	slash1 = strchr(base, '/');
	if (!slash1)
		return false;
	*slash1 = '\0';
	piddir = base;
	if (!vns_component_is_pid_dir(piddir))
		return false;

	slash1++;
	if (strcmp(slash1, "ns/" VNS_PROC_NS_IPC_NAME))
		return false;

	*rpid = vns_resolve_ns_ipc_pid(piddir);
	return *rpid > 0;
}

/*
 * vns_ns_ipc_readlink() - fabricate the "ipc:[<ino>]" symlink target text
 * real readlink(2) on .../ns/ipc would return, for the task named by @rpid.
 * Returns the string length copied (>= 0) on success, or a negative errno.
 */
static long vns_ns_ipc_readlink(pid_t rpid, char __user *ubuf, int bufsiz)
{
	struct pid *pid;
	struct task_struct *task;
	struct ipc_namespace *ns;
	char name[32];
	int n;

	if (bufsiz < 0)
		return -EINVAL;

	pid = find_get_pid(rpid);
	if (!pid)
		return -ENOENT;

	task = get_pid_task(pid, PIDTYPE_PID);
	put_pid(pid);
	if (!task)
		return -ENOENT;

	/*
	 * vns_task_ipc_ns() returns a borrowed pointer into
	 * task->nsproxy->ipc_ns with no reference of its own, and reads
	 * task->nsproxy without task_lock(). Both the pointer read and the
	 * ref-get must happen under task_lock(task), the same lock every
	 * nsproxy-swapping path (vns_task_exit_cleanup(), unshare(2), setns(2))
	 * takes before replacing/freeing task->nsproxy -- otherwise @task
	 * could swap/free its nsproxy concurrently between the lookup above
	 * and the vns_ipc_get_ref() below, leaving @ns dangling. The real
	 * kernel's get_ipc_ns()/put_ipc_ns() are also no-ops when
	 * CONFIG_IPC_NS=n (see <linux/ipc_namespace.h>) -- exactly the config
	 * this whole file exists for -- so vns_ipc_get_ref()/vns_put_ipc_ns()
	 * (the same refcount vendor_kernel's own put path already maintains
	 * unconditionally) must be used instead.
	 */
	task_lock(task);
	ns = vns_task_ipc_ns(task);
	if (ns)
		vns_ipc_get_ref(ns);
	task_unlock(task);
	put_task_struct(task);
	if (!ns)
		return -ENOENT;

	n = snprintf(name, sizeof(name), "%s:[%u]", VNS_PROC_NS_IPC_NAME,
		     ns->ns.inum);
	vns_put_ipc_ns(ns);
	if (n < 0)
		return -ENOENT;

	if (bufsiz > n)
		bufsiz = n;
	if (copy_to_user(ubuf, name, bufsiz))
		return -EFAULT;
	return bufsiz;
}

static long (*real_sys_readlinkat)(const struct pt_regs *regs);
static long (*real_sys_readlink)(const struct pt_regs *regs);

static long vendor_kernel_hook_readlinkat(const struct pt_regs *regs)
{
	int dfd = (int)vns_procfs_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg1(regs);
	char __user *ubuf =
		(char __user *)(uintptr_t)vns_procfs_arg2(regs);
	int bufsiz = (int)vns_procfs_arg3(regs);
	long ret;
	pid_t rpid;

	ret = real_sys_readlinkat(regs);
	if (ret != -ENOENT)
		return ret;

	if (!vns_path_is_ns_ipc(dfd, upath, &rpid))
		return ret;

	return vns_ns_ipc_readlink(rpid, ubuf, bufsiz);
}

static long vendor_kernel_hook_readlink(const struct pt_regs *regs)
{
	const char __user *upath =
		(const char __user *)(uintptr_t)vns_procfs_arg0(regs);
	char __user *ubuf =
		(char __user *)(uintptr_t)vns_procfs_arg1(regs);
	int bufsiz = (int)vns_procfs_arg2(regs);
	long ret;
	pid_t rpid;

	ret = real_sys_readlink(regs);
	if (ret != -ENOENT)
		return ret;

	if (!vns_path_is_ns_ipc(AT_FDCWD, upath, &rpid))
		return ret;

	return vns_ns_ipc_readlink(rpid, ubuf, bufsiz);
}

static const char * const vendor_kernel_readlinkat_names[] = {
	"__arm64_sys_readlinkat", "__x64_sys_readlinkat", "sys_readlinkat",
	NULL,
};
static const char * const vendor_kernel_readlink_names[] = {
	"__arm64_sys_readlink", "__x64_sys_readlink", "sys_readlink", NULL,
};

static struct shadow_hook vendor_kernel_readlinkat_hook =
	SHADOW_HOOK(vendor_kernel_readlinkat_names, vendor_kernel_hook_readlinkat,
		    &real_sys_readlinkat);
static struct shadow_hook vendor_kernel_readlink_hook =
	SHADOW_HOOK(vendor_kernel_readlink_names, vendor_kernel_hook_readlink,
		    &real_sys_readlink);

struct shadow_hook *vendor_kernel_procfs_hooks[] = {
	&vendor_kernel_readlinkat_hook,
	&vendor_kernel_readlink_hook,
	NULL,
};
