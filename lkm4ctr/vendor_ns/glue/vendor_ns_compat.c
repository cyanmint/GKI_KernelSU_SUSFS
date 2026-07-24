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
#include <linux/security.h>
#include <linux/perf_event.h>
#include <linux/ipc_namespace.h>
#include <linux/mnt_namespace.h>
#include <linux/ns_common.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/file.h>

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
#ifdef CONFIG_SECURITY
int security_create_user_ns(const struct cred *cred)
{
	if (vns_security_create_user_ns_real)
		return vns_security_create_user_ns_real(cred);
	return 0; /* stub: allow all */
}
#endif

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
#ifdef CONFIG_POSIX_MQUEUE
bool setup_mq_sysctls(struct ipc_namespace *ns)
{
	if (vns_setup_mq_sysctls_real)
		return vns_setup_mq_sysctls_real(ns);
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
#endif /* CONFIG_POSIX_MQUEUE */

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
 * pidfds (it will still work with /proc/<pid>/ns/* paths).
 */
struct pid *pidfd_pid(const struct file *file)
{
	if (vns_pidfd_pid_real)
		return vns_pidfd_pid_real(file);
	return ERR_PTR(-EBADF); /* stub */
}
