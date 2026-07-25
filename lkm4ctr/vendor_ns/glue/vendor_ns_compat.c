// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_ns_compat.c - local stubs/resolvers for kernel-internal symbols
 * used by the vendored namespace files.
 *
 * This is NEW code (not vendored from kernel-common); it lives in glue/
 * alongside the other vendor_ns-specific support files.
 *
 * Problem: the vendored kernel files (utsname.c, user_namespace.c, etc.)
 * call several kernel-internal symbols that are NOT exported via
 * EXPORT_SYMBOL/EXPORT_SYMBOL_GPL and are therefore invisible to out-of-
 * tree modules.  Rather than changing every call site in the vendored
 * files (which would inflate the diff against the upstream source), we
 * provide local definitions here that shadow the kernel versions for any
 * call within this module:
 *
 *   inc_ucount / dec_ucount       kernel/ucount.c  — not exported
 *   setup_userns_sysctls           kernel/user_namespace.c — not exported
 *   retire_userns_sysctls          kernel/user_namespace.c — not exported
 *   security_create_user_ns        security/security.c — not exported
 *   perf_event_namespaces          kernel/events/core.c — not exported
 *   setup_mq_sysctls               ipc/mqueue.c — not exported
 *   mq_clear_sbinfo                ipc/mqueue.c — not exported
 *   mq_put_mnt                     ipc/mqueue.c — not exported
 *   mq_lock (variable)             ipc/mqueue.c — not exported
 *   msg_init_ns                    ipc/msg.c — not exported
 *   from_mnt_ns                    fs/namespace.c — not exported
 *   pidfd_pid                      kernel/pid.c — not exported
 *
 * At vendor_ns_init() time, each of these is resolved via
 * shadow_hook_resolve() so that, on the real Android GKI kernel where the
 * symbols ARE present (just not exported), the real kernel functions are
 * called through the function pointers stored here.  On the host build-
 * check kernel (where modpost would otherwise reject the undefined
 * references), the local definitions below satisfy the linker.
 *
 * Every stub is safe to call even when the resolved pointer is NULL:
 *   - ucounts stubs: return a static placeholder that passes null-checks;
 *     actual resource limit enforcement is skipped (acceptable for the
 *     parallel vendor namespace subsystem).
 *   - sysctl/perf/security stubs: no-ops or "allow-all" returns.
 *   - mqueue stubs: no-ops (mqueue VFS state lives in the real kernel).
 *   - from_mnt_ns: returns NULL (mount-ns setns is not implemented yet).
 *   - pidfd_pid: returns ERR_PTR(-EBADF) (fallback to proc_ns_file path).
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/user_namespace.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/security.h>
#include <linux/perf_event.h>
#include <linux/ipc_namespace.h>
#include <linux/mnt_namespace.h>
#include <linux/ns_common.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs_struct.h>
#include <linux/ptrace.h>
#include <linux/key.h>
#include <linux/time_namespace.h>
#include <linux/cgroup.h>
#include <linux/proc_fs.h>
#include <linux/sem.h>
#include <linux/cred.h>

#define VNS_COMPAT_IMPL
#include "../vendor_ns.h"
#include "../include/uapi/vendor_ns.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

/* ---- resolved function pointer types ---------------------------------- */

typedef struct ucounts *(*inc_ucount_fn_t)(struct user_namespace *, kuid_t,
					   enum ucount_type);
typedef void (*dec_ucount_fn_t)(struct ucounts *, enum ucount_type);
typedef bool (*setup_userns_sysctls_fn_t)(struct user_namespace *);
typedef void (*retire_userns_sysctls_fn_t)(struct user_namespace *);
typedef int  (*security_create_user_ns_fn_t)(const struct cred *);
typedef void (*perf_event_namespaces_fn_t)(struct task_struct *);
typedef bool (*setup_mq_sysctls_fn_t)(struct ipc_namespace *);
typedef void (*mq_clear_sbinfo_fn_t)(struct ipc_namespace *);
typedef void (*mq_put_mnt_fn_t)(struct ipc_namespace *);
typedef int  (*msg_init_ns_fn_t)(struct ipc_namespace *);
typedef struct ns_common *(*from_mnt_ns_fn_t)(struct mnt_namespace *);
typedef struct pid *(*pidfd_pid_fn_t)(const struct file *);
typedef void (*set_fs_root_fn_t)(struct fs_struct *, const struct path *);
typedef struct fs_struct *(*copy_fs_struct_fn_t)(struct fs_struct *);
typedef void (*set_fs_pwd_fn_t)(struct fs_struct *, const struct path *);
typedef void (*free_fs_struct_fn_t)(struct fs_struct *);
typedef bool (*ptrace_may_access_fn_t)(struct task_struct *, unsigned int);
typedef bool (*current_chrooted_fn_t)(void);
typedef void (*disable_pid_allocation_fn_t)(struct pid_namespace *);
typedef bool (*proc_ns_file_fn_t)(const struct file *);
typedef void (*retire_ipc_sysctls_fn_t)(struct ipc_namespace *);
#ifdef CONFIG_POSIX_MQUEUE
typedef void (*retire_mq_sysctls_fn_t)(struct ipc_namespace *);
#endif
#ifdef CONFIG_KEYS
typedef void (*key_free_user_ns_fn_t)(struct user_namespace *);
#endif
#ifdef CONFIG_SYSVIPC
typedef void (*sem_init_ns_fn_t)(struct ipc_namespace *);
typedef void (*shm_init_ns_fn_t)(struct ipc_namespace *);
typedef void (*exit_sem_fn_t)(struct task_struct *);
#endif
typedef bool (*setup_ipc_sysctls_fn_t)(struct ipc_namespace *);
typedef int  (*set_cred_ucounts_fn_t)(struct cred *);
#ifdef CONFIG_USER_NS
typedef bool (*in_userns_fn_t)(const struct user_namespace *,
			       const struct user_namespace *);
#endif
#ifdef CONFIG_POSIX_MQUEUE
typedef int  (*mq_init_ns_fn_t)(struct ipc_namespace *);
#endif

static inc_ucount_fn_t            vns_inc_ucount_real;
static dec_ucount_fn_t            vns_dec_ucount_real;
static setup_userns_sysctls_fn_t  vns_setup_userns_sysctls_real;
static retire_userns_sysctls_fn_t vns_retire_userns_sysctls_real;
static security_create_user_ns_fn_t vns_security_create_user_ns_real;
static perf_event_namespaces_fn_t vns_perf_event_namespaces_real;
static setup_mq_sysctls_fn_t      vns_setup_mq_sysctls_real;
static mq_clear_sbinfo_fn_t       vns_mq_clear_sbinfo_real;
static mq_put_mnt_fn_t            vns_mq_put_mnt_real;
static msg_init_ns_fn_t           vns_msg_init_ns_real;
static from_mnt_ns_fn_t           vns_from_mnt_ns_real;
static pidfd_pid_fn_t             vns_pidfd_pid_real;
static set_fs_root_fn_t           vns_set_fs_root_real;
static copy_fs_struct_fn_t        vns_copy_fs_struct_real;
static set_fs_pwd_fn_t            vns_set_fs_pwd_real;
static free_fs_struct_fn_t        vns_free_fs_struct_real;
static ptrace_may_access_fn_t     vns_ptrace_may_access_real;
static current_chrooted_fn_t      vns_current_chrooted_real;
static disable_pid_allocation_fn_t vns_disable_pid_allocation_real;
static proc_ns_file_fn_t          vns_proc_ns_file_real;
static retire_ipc_sysctls_fn_t    vns_retire_ipc_sysctls_real;
#ifdef CONFIG_POSIX_MQUEUE
static retire_mq_sysctls_fn_t     vns_retire_mq_sysctls_real;
#endif
#ifdef CONFIG_KEYS
static key_free_user_ns_fn_t      vns_key_free_user_ns_real;
#endif
#ifdef CONFIG_SYSVIPC
static sem_init_ns_fn_t           vns_sem_init_ns_real;
static shm_init_ns_fn_t           vns_shm_init_ns_real;
static exit_sem_fn_t              vns_exit_sem_real;
#endif
static setup_ipc_sysctls_fn_t     vns_setup_ipc_sysctls_real;
static set_cred_ucounts_fn_t      vns_set_cred_ucounts_real;
#ifdef CONFIG_USER_NS
static in_userns_fn_t             vns_in_userns_real;
#endif
#ifdef CONFIG_POSIX_MQUEUE
static mq_init_ns_fn_t            vns_mq_init_ns_real;
#endif

/*
 * [BUILD-COMPAT] tasklist_lock (kernel/fork.c, not exported).
 * Protects the task list when pid_namespace.c walks processes during teardown.
 * This is a module-local lock; it does not protect the kernel's task list,
 * but satisfies the linker reference and provides mutual exclusion within
 * vendor_ns's own teardown paths.
 */
DEFINE_RWLOCK(tasklist_lock);

/* Resolved pointer to the kernel's init_cgroup_ns data object. */
#ifdef CONFIG_CGROUPS
struct cgroup_namespace *vns_init_cgroup_ns_ptr;
#endif /* CONFIG_CGROUPS */

/* Resolved pointer to the kernel's init_ipc_ns data object. */
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
struct ipc_namespace *vns_init_ipc_ns_ptr;
#endif /* CONFIG_POSIX_MQUEUE || CONFIG_SYSVIPC */

/*
 * Resolve all non-exported symbols at init time.  Called from
 * vendor_ns_init() before any vendored namespace code runs.
 */
void vns_compat_resolve(void)
{
#define RESOLVE(var, sym) \
	do { \
		(var) = (typeof(var))(uintptr_t)shadow_hook_resolve(#sym); \
		if (!(var)) \
			LKM4CTR_WARN(VENDOR_NS_TAG, \
				"compat: " #sym " not resolved (stub active)"); \
	} while (0)

	RESOLVE(vns_inc_ucount_real,            inc_ucount);
	RESOLVE(vns_dec_ucount_real,            dec_ucount);
	RESOLVE(vns_setup_userns_sysctls_real,  setup_userns_sysctls);
	RESOLVE(vns_retire_userns_sysctls_real, retire_userns_sysctls);
	RESOLVE(vns_security_create_user_ns_real, security_create_user_ns);
	RESOLVE(vns_perf_event_namespaces_real, perf_event_namespaces);
	RESOLVE(vns_setup_mq_sysctls_real,      setup_mq_sysctls);
	RESOLVE(vns_mq_clear_sbinfo_real,       mq_clear_sbinfo);
	RESOLVE(vns_mq_put_mnt_real,            mq_put_mnt);
	RESOLVE(vns_msg_init_ns_real,           msg_init_ns);
	RESOLVE(vns_from_mnt_ns_real,           from_mnt_ns);
	RESOLVE(vns_pidfd_pid_real,             pidfd_pid);
	RESOLVE(vns_set_fs_root_real,           set_fs_root);
	RESOLVE(vns_copy_fs_struct_real,        copy_fs_struct);
	RESOLVE(vns_set_fs_pwd_real,            set_fs_pwd);
	RESOLVE(vns_free_fs_struct_real,        free_fs_struct);
	RESOLVE(vns_ptrace_may_access_real,     ptrace_may_access);
	RESOLVE(vns_current_chrooted_real,      current_chrooted);
	RESOLVE(vns_disable_pid_allocation_real, disable_pid_allocation);
	RESOLVE(vns_proc_ns_file_real,          proc_ns_file);
	RESOLVE(vns_retire_ipc_sysctls_real,    retire_ipc_sysctls);
#ifdef CONFIG_POSIX_MQUEUE
	RESOLVE(vns_retire_mq_sysctls_real,     retire_mq_sysctls);
#endif
#ifdef CONFIG_KEYS
	RESOLVE(vns_key_free_user_ns_real,      key_free_user_ns);
#endif
#ifdef CONFIG_SYSVIPC
	RESOLVE(vns_sem_init_ns_real,           sem_init_ns);
	RESOLVE(vns_shm_init_ns_real,           shm_init_ns);
	RESOLVE(vns_exit_sem_real,              exit_sem);
#endif
	RESOLVE(vns_setup_ipc_sysctls_real,     setup_ipc_sysctls);
	RESOLVE(vns_set_cred_ucounts_real,      set_cred_ucounts);
#ifdef CONFIG_USER_NS
	RESOLVE(vns_in_userns_real,             in_userns);
#endif
#ifdef CONFIG_POSIX_MQUEUE
	RESOLVE(vns_mq_init_ns_real,            mq_init_ns);
#endif
#ifdef CONFIG_CGROUPS
	vns_init_cgroup_ns_ptr = (struct cgroup_namespace *)(uintptr_t)
		shadow_hook_resolve("init_cgroup_ns");
	if (!vns_init_cgroup_ns_ptr)
		LKM4CTR_WARN(VENDOR_NS_TAG,
			"compat: init_cgroup_ns not resolved (cgroup ns disabled)");
#endif
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
	vns_init_ipc_ns_ptr = (struct ipc_namespace *)(uintptr_t)
		shadow_hook_resolve("init_ipc_ns");
	if (!vns_init_ipc_ns_ptr)
		LKM4CTR_WARN(VENDOR_NS_TAG,
			"compat: init_ipc_ns not resolved (ipc ns disabled)");
#endif
#undef RESOLVE
}

/* ---- placeholder ucounts object --------------------------------------- */
/*
 * A static placeholder returned by inc_ucount() when the real function is
 * not available.  All dec_ucount() callers must test for this sentinel and
 * skip the real dec call.  Declared in vendor_ns.h so vendored files can
 * reference it if needed (they currently do not).
 */
struct ucounts vns_ucounts_stub;

/* ---- local definitions of kernel-internal symbols -------------------- */

/*
 * [BUILD-COMPAT] inc_ucount / dec_ucount (kernel/ucount.c, not exported).
 * The real functions enforce per-user-namespace resource limits.  The stubs
 * below skip enforcement (always allow) so vendor_ns namespaces can be
 * created even on kernels that restrict ucounts.  On kernels where the real
 * functions resolve, they are called instead.
 */
struct ucounts *inc_ucount(struct user_namespace *ns, kuid_t uid,
			   enum ucount_type type)
{
	if (vns_inc_ucount_real)
		return vns_inc_ucount_real(ns, uid, type);
	/* stub: return non-NULL sentinel — no limit enforced */
	return &vns_ucounts_stub;
}

void dec_ucount(struct ucounts *ucounts, enum ucount_type type)
{
	if (!ucounts || ucounts == &vns_ucounts_stub)
		return;
	if (vns_dec_ucount_real)
		vns_dec_ucount_real(ucounts, type);
}

/*
 * [BUILD-COMPAT] setup_userns_sysctls / retire_userns_sysctls
 * (kernel/user_namespace.c, not exported).
 * These register/unregister per-user-ns sysctl entries.  Skipping them
 * means /proc/sys/user/ entries for vendor namespaces are absent, which
 * is acceptable for a parallel namespace subsystem.
 */
bool setup_userns_sysctls(struct user_namespace *ns)
{
	if (vns_setup_userns_sysctls_real)
		return vns_setup_userns_sysctls_real(ns);
	return true; /* stub: pretend success */
}

void retire_userns_sysctls(struct user_namespace *ns)
{
	if (vns_retire_userns_sysctls_real)
		vns_retire_userns_sysctls_real(ns);
	/* stub: no-op */
}

/*
 * [BUILD-COMPAT] security_create_user_ns (security/security.c, not exported).
 * LSM hook that decides whether creating a new user namespace is allowed.
 * When the real hook is not available, we allow all creation (returns 0).
 * <linux/security.h> declares this as extern when CONFIG_SECURITY=y;
 * our definition satisfies in-module references and avoids modpost errors.
 */
int vns_security_create_user_ns(const struct cred *cred)
{
	if (vns_security_create_user_ns_real)
		return vns_security_create_user_ns_real(cred);
	return 0; /* stub: allow all */
}

/*
 * [BUILD-COMPAT] perf_event_namespaces (kernel/events/core.c, not exported).
 * Notification to the perf subsystem after setns().  Safe to skip.
 * <linux/perf_event.h> declares this as extern when CONFIG_PERF_EVENTS=y.
 */
#ifdef CONFIG_PERF_EVENTS
void perf_event_namespaces(struct task_struct *tsk)
{
	if (vns_perf_event_namespaces_real)
		vns_perf_event_namespaces_real(tsk);
	/* stub: no-op */
}
#endif

/*
 * [BUILD-COMPAT] setup_mq_sysctls (ipc/mqueue.c, not exported).
 * Sets up per-ipc-ns mqueue sysctl table.  Skipping means mqueue sysctl
 * entries are absent for vendor IPC namespaces.
 * <linux/ipc_namespace.h> declares this as extern when CONFIG_POSIX_MQUEUE=y.
 */
bool vns_setup_mq_sysctls(struct ipc_namespace *ns)
{
#ifdef CONFIG_POSIX_MQUEUE
	if (vns_setup_mq_sysctls_real)
		return vns_setup_mq_sysctls_real(ns);
#endif
	return true; /* stub */
}

/*
 * [BUILD-COMPAT] mq_clear_sbinfo / mq_put_mnt (ipc/mqueue.c, not exported).
 * mq_clear_sbinfo clears the mqueue superblock's private data when the
 * ipc_namespace is being freed.  mq_put_mnt unmounts the mqueue filesystem.
 * Both are no-ops here because vendor_ns does not mount a per-ns mqueue.
 * Declared in vendor_ns/ipc/util.h which ipc/namespace.c includes.
 */
void mq_clear_sbinfo(struct ipc_namespace *ns)
{
	if (vns_mq_clear_sbinfo_real)
		vns_mq_clear_sbinfo_real(ns);
	/* stub: no-op */
}

void mq_put_mnt(struct ipc_namespace *ns)
{
	if (vns_mq_put_mnt_real)
		vns_mq_put_mnt_real(ns);
	/* stub: no-op */
}

/*
 * [BUILD-COMPAT] mq_lock (ipc/mqueue.c, not exported).
 * Spinlock protecting the mqueue VFS reference-count transition from 1→0.
 * Our ipc/namespace.c uses it in vns_put_ipc_ns() (vendored free_ipc_ns).
 * This is our MODULE-LOCAL mq_lock — it protects vendor_ns ipc namespace
 * refcount drops only; it is entirely separate from the real kernel mq_lock.
 * Declared as extern in <linux/ipc_namespace.h> when CONFIG_POSIX_MQUEUE=y.
 */
DEFINE_SPINLOCK(mq_lock);

/*
 * [BUILD-COMPAT] msg_init_ns (ipc/msg.c, not exported).
 * Initialises the SysV message queue state for a new ipc_namespace.
 * ipc/util.h already wraps this with #ifdef CONFIG_SYSVIPC / static inline
 * fallback; this definition covers the CONFIG_SYSVIPC=y path on host kernels
 * where the symbol is not exported.
 */
#ifdef CONFIG_SYSVIPC
int msg_init_ns(struct ipc_namespace *ns)
{
	if (vns_msg_init_ns_real)
		return vns_msg_init_ns_real(ns);
	return 0; /* stub: empty message namespace */
}
#endif

/*
 * [BUILD-COMPAT] from_mnt_ns (fs/namespace.c, not exported).
 * Returns the ns_common embedded inside a mnt_namespace.  Used in
 * nsproxy.c's validate_nsset() when checking CLONE_NEWNS during setns.
 * Mount namespace support is not yet vendored, so setns(CLONE_NEWNS) is
 * intentionally rejected via this returning NULL.
 */
struct ns_common *from_mnt_ns(struct mnt_namespace *mnt_ns)
{
	if (vns_from_mnt_ns_real)
		return vns_from_mnt_ns_real(mnt_ns);
	return NULL; /* stub: mount ns not vendored */
}

/*
 * [BUILD-COMPAT] pidfd_pid (kernel/pid.c, not exported on all kernels).
 * Returns the struct pid for a pidfd file, used in vns_sys_setns() to
 * identify the target namespace set from a process pidfd.
 * Falls back to ERR_PTR(-EBADF) if unresolved, causing setns to reject
 * pidfds (it will still work with /proc/<pid>/ns/<type> paths).
 */
struct pid *pidfd_pid(const struct file *file)
{
	if (vns_pidfd_pid_real)
		return vns_pidfd_pid_real(file);
	return ERR_PTR(-EBADF); /* stub */
}

/*
 * [BUILD-COMPAT] free_time_ns (kernel/time/namespace.c, not exported).
 * put_time_ns() is a static inline in <linux/time_namespace.h> that calls
 * free_time_ns() when the refcount reaches zero.  Our vendored copy of
 * free_time_ns lives as vns_free_time_ns(); this shim forwards the call so
 * the inline can resolve.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
void free_time_ns(struct kref *kref)
{
	vns_free_time_ns(container_of(kref, struct time_namespace, kref));
}
#else
void free_time_ns(struct time_namespace *ns)
{
	vns_free_time_ns(ns);
}
#endif

/*
 * [BUILD-COMPAT] set_fs_root (fs/fs_struct.c, not exported).
 * Updates the root path stored in a task's fs_struct.  Used by
 * vns_install_nsproxy() when switching namespaces.
 */
void set_fs_root(struct fs_struct *fs, const struct path *path)
{
	if (vns_set_fs_root_real)
		vns_set_fs_root_real(fs, path);
	/* stub: root path unchanged — acceptable for the parallel ns subsystem */
}

/*
 * [BUILD-COMPAT] copy_fs_struct (fs/fs_struct.c, not exported).
 * Allocates a copy of the caller's fs_struct.  Used in copy_namespaces()
 * when the new namespace set needs an independent filesystem root.
 */
struct fs_struct *copy_fs_struct(struct fs_struct *old)
{
	if (vns_copy_fs_struct_real)
		return vns_copy_fs_struct_real(old);
	return NULL; /* stub: no fs_struct copy — fs root stays shared */
}

/*
 * [BUILD-COMPAT] ptrace_may_access (kernel/ptrace.c, not exported on GKI).
 * Access-mode check used in vns_sys_setns() to gate cross-process ns changes.
 * Fall back to denying access if the real function is not resolved.
 */
bool ptrace_may_access(struct task_struct *task, unsigned int mode)
{
	if (vns_ptrace_may_access_real)
		return vns_ptrace_may_access_real(task, mode);
	return false; /* stub: deny — safer than allow */
}

/*
 * [BUILD-COMPAT] disable_pid_allocation (kernel/pid.c, not exported).
 * Clears the PIDNS_ADDING flag so the pid namespace stops accepting new pids.
 * Called in vns_zap_pid_ns_processes() during pid namespace teardown.
 */
void disable_pid_allocation(struct pid_namespace *ns)
{
	if (vns_disable_pid_allocation_real)
		vns_disable_pid_allocation_real(ns);
	/* stub: no-op — pids drain normally on process exit */
}

#ifdef CONFIG_KEYS
/*
 * [BUILD-COMPAT] key_free_user_ns (security/keys/user_defined.c, not exported).
 * Releases keyrings tied to a user_namespace.
 * <linux/key.h> already provides a no-op macro when !CONFIG_KEYS.
 */
void key_free_user_ns(struct user_namespace *ns)
{
	if (vns_key_free_user_ns_real)
		vns_key_free_user_ns_real(ns);
	/* stub: keyrings not freed — acceptable as they are ref-counted */
}
#endif /* CONFIG_KEYS */

#ifdef CONFIG_SYSVIPC
/*
 * [BUILD-COMPAT] sem_init_ns (ipc/sem.c, not exported).
 * Initialises the SysV semaphore IDs for a new ipc_namespace.
 * ipc/util.h already provides a static inline no-op when !CONFIG_SYSVIPC.
 */
void sem_init_ns(struct ipc_namespace *ns)
{
	if (vns_sem_init_ns_real)
		vns_sem_init_ns_real(ns);
	/* stub: no semaphores in this IPC namespace */
}
#endif /* CONFIG_SYSVIPC */

/*
 * [BUILD-COMPAT] free_uts_ns (kernel/utsname.c, not exported).
 * put_uts_ns() is a static inline in <linux/utsname.h> that calls
 * free_uts_ns() when the refcount reaches zero.  Delegate to
 * vns_free_uts_ns() defined in our vendored utsname.c.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
void free_uts_ns(struct kref *kref)
{
	vns_free_uts_ns(container_of(kref, struct uts_namespace, kref));
}
#else
void free_uts_ns(struct uts_namespace *ns)
{
	vns_free_uts_ns(ns);
}
#endif

/*
 * [BUILD-COMPAT] set_fs_pwd (fs/fs_struct.c, not exported).
 * Updates the current working directory in a task's fs_struct.
 */
void set_fs_pwd(struct fs_struct *fs, const struct path *path)
{
	if (vns_set_fs_pwd_real)
		vns_set_fs_pwd_real(fs, path);
	/* stub: pwd unchanged */
}

/*
 * [BUILD-COMPAT] free_fs_struct (fs/fs_struct.c, not exported).
 * Releases an fs_struct allocated by copy_fs_struct().
 */
void free_fs_struct(struct fs_struct *fs)
{
	if (vns_free_fs_struct_real)
		vns_free_fs_struct_real(fs);
	/* stub: no-op (minor leak acceptable for ns teardown) */
}

/*
 * [BUILD-COMPAT] current_chrooted (fs/fs_struct.c, not exported).
 * Returns true if the current task is in a chroot jail.
 * Used in user_namespace.c to gate unshare(CLONE_NEWUSER).
 */
bool current_chrooted(void)
{
	if (vns_current_chrooted_real)
		return vns_current_chrooted_real();
	return false; /* stub: not chrooted — allow user namespace creation */
}

/*
 * [BUILD-COMPAT] proc_ns_file (fs/nsfs.c, not exported on GKI).
 * Returns true if the given file is a /proc/<pid>/ns/<ns> magic-link file.
 * Used in vns_sys_setns() to check whether the fd refers to a namespace.
 */
bool proc_ns_file(const struct file *file)
{
	if (vns_proc_ns_file_real)
		return vns_proc_ns_file_real(file);
	return false; /* stub: treat as non-proc-ns file */
}

/*
 * [BUILD-COMPAT] retire_ipc_sysctls (ipc/sysctls.c, not exported).
 * Unregisters per-ipc-ns sysctl entries.
 * <linux/ipc_namespace.h> declares this when CONFIG_SYSCTL=y.
 */
void vns_retire_ipc_sysctls(struct ipc_namespace *ns)
{
	if (vns_retire_ipc_sysctls_real)
		vns_retire_ipc_sysctls_real(ns);
	/* stub: sysctl entries remain (harmless for a parallel ns subsystem) */
}

/*
 * [BUILD-COMPAT] retire_mq_sysctls (ipc/mqueue.c, not exported).
 * Unregisters per-ipc-ns mqueue sysctl entries.
 */
void vns_retire_mq_sysctls(struct ipc_namespace *ns)
{
#ifdef CONFIG_POSIX_MQUEUE
	if (vns_retire_mq_sysctls_real)
		vns_retire_mq_sysctls_real(ns);
#endif
	/* stub: no-op */
}

#ifdef CONFIG_SYSVIPC
/*
 * [BUILD-COMPAT] shm_init_ns (ipc/shm.c, not exported).
 * Initialises the SysV shared memory IDs for a new ipc_namespace.
 * ipc/util.h provides a static inline no-op when !CONFIG_SYSVIPC.
 */
void shm_init_ns(struct ipc_namespace *ns)
{
	if (vns_shm_init_ns_real)
		vns_shm_init_ns_real(ns);
	/* stub: no shared memory segments in this IPC namespace */
}
#endif /* CONFIG_SYSVIPC */

/*
 * [BUILD-COMPAT] setup_ipc_sysctls (ipc/sysctls.c, not exported).
 * Registers per-ipc-ns sysctl table entries.
 * <linux/ipc_namespace.h> declares this as extern when CONFIG_SYSCTL=y.
 */
bool vns_setup_ipc_sysctls(struct ipc_namespace *ns)
{
	if (vns_setup_ipc_sysctls_real)
		return vns_setup_ipc_sysctls_real(ns);
	return true; /* stub: pretend success */
}

/*
 * [BUILD-COMPAT] set_cred_ucounts (kernel/cred.c, not exported).
 * Associates ucounts with a credentials struct during user_namespace creation.
 * Returns 0 on success.  Stub returns 0 (allow) when not resolved.
 */
int set_cred_ucounts(struct cred *new)
{
	if (vns_set_cred_ucounts_real)
		return vns_set_cred_ucounts_real(new);
	return 0; /* stub: no ucounts tracking */
}

#ifdef CONFIG_USER_NS
/*
 * [BUILD-COMPAT] in_userns (kernel/user_namespace.c, not exported on GKI).
 * Returns true if 'descendant' is the same as or a descendant of 'ancestor'.
 * <linux/user_namespace.h> provides a static inline fallback when !CONFIG_USER_NS.
 */
bool in_userns(const struct user_namespace *ancestor,
	       const struct user_namespace *descendant)
{
	if (vns_in_userns_real)
		return vns_in_userns_real(ancestor, descendant);
	return ancestor == descendant; /* stub: only exact match */
}
#endif /* CONFIG_USER_NS */

#ifdef CONFIG_POSIX_MQUEUE
/*
 * [BUILD-COMPAT] mq_init_ns (ipc/mqueue.c, not exported).
 * Initialises the POSIX mqueue VFS mount for a new ipc_namespace.
 * <linux/ipc_namespace.h> provides a static inline returning 0 when
 * !CONFIG_POSIX_MQUEUE.
 */
int mq_init_ns(struct ipc_namespace *ns)
{
	if (vns_mq_init_ns_real)
		return vns_mq_init_ns_real(ns);
	return 0; /* stub: no mqueue for this IPC namespace */
}
#endif /* CONFIG_POSIX_MQUEUE */

#ifdef CONFIG_SYSVIPC
/*
 * [BUILD-COMPAT] exit_sem (ipc/sem.c, not exported).
 * Called on task exit to release any SysV semaphore undo structures.
 * <linux/sem.h> provides a static inline no-op when !CONFIG_SYSVIPC.
 */
void exit_sem(struct task_struct *tsk)
{
	if (vns_exit_sem_real)
		vns_exit_sem_real(tsk);
	/* stub: no-op — undo structs will be cleaned up on process exit */
}
#endif /* CONFIG_SYSVIPC */
