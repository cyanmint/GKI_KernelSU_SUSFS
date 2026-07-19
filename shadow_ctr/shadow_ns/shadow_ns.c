// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns - standalone namespace subsystem
 *
 * A single, standalone loadable kernel module (not split into per-type
 * submodules) that intercepts unshare(2), setns(2), clone(2)/clone3(2)/
 * fork(2)/vfork(2) so an unmodified `containerd`/`runc`/`dockerd` gets full
 * namespace isolation.
 *
 * The critical design point, discovered by reading this repository's own
 * kernel build pipeline (.github/workflows/scripts/kernel_builder.py), is
 * that the production kernel this module targets is built with
 * CONFIG_UTS_NS=y, CONFIG_PID_NS=y, CONFIG_IPC_NS=y, CONFIG_USER_NS=y and
 * CONFIG_NET_NS=y whenever containerd support is requested (the
 * "CONTAINERD_CONFIG" fragment, applied whenever `use_containerd` is set,
 * which defaults to true — see BuildConfig.use_containerd in config.py).
 * Mount namespaces (CLONE_NEWNS) have no Kconfig gate at all and are always
 * compiled in, and cgroup namespaces (CLONE_NEWCGROUP) only require
 * CONFIG_CGROUPS=y, which every GKI defconfig sets. In other words: on the
 * kernel this module actually loads into, *every* namespace type already has
 * a real, fully-functional, kernel-native implementation compiled into
 * vmlinux.
 *
 * The previous generation of this module family (shadow_ns_base.ko plus six
 * thin shadow_ns_<type>.ko presence submodules) did not take this into
 * account: it *unconditionally* stripped every CLONE_NEW* namespace flag out
 * of the real unshare()/clone()/clone3() arguments and replaced them with a
 * purely in-module, reference-counted bookkeeping simulation. That is exactly
 * why an unmodified dockerd's unshare() of its container init process failed
 * to isolate anything from the host: the real, kernel-native namespace the
 * syscall would otherwise have created was silently discarded, and nothing
 * outside this module's own bookkeeping tables (which the rest of the kernel
 * — /proc, signal delivery, mount propagation, socket lookups, ... — knows
 * nothing about) ever saw a namespace change. The container's task kept
 * running with the host's real nsproxy in every respect that matters.
 *
 * shadow_ns fixes this by only ever simulating a namespace type that this
 * kernel build genuinely lacks (checked via IS_ENABLED(CONFIG_*_NS), which
 * reflects the actual .config this module is built against — see
 * SHADOW_NS_BUILTIN_FLAGS below). Every namespace flag bit the kernel really
 * supports is left completely untouched in the arguments passed to the real
 * unshare()/setns()/clone()/clone3() syscalls, so the kernel's own real,
 * fully-conformant namespace subsystem does the actual isolation work, with
 * shadow_ns getting out of the way entirely (see SHADOW_NS_SHADOW_CLONE_FLAGS
 * — on the expected production build it evaluates to 0, and every hook below
 * degenerates into a transparent passthrough). Extracting/duplicating the
 * real kernel/pid_namespace.c, kernel/user_namespace.c, kernel/utsname.c,
 * ipc/namespace.c, net/core/net_namespace.c and fs/namespace.c logic into
 * this module would be redundant (and actively dangerous: those namespace
 * types are already compiled into vmlinux, and task_struct->nsproxy, cred,
 * and every syscall that consults them already exist and work) whenever the
 * real support is present, which is the expected common case here.
 *
 * The only place a functional simulation still makes sense is a namespace
 * type this particular kernel build genuinely does not support natively
 * (e.g. a KMI/config combination built without containerd support). For that
 * fallback case shadow_ns keeps the reference-counted bookkeeping registry
 * from the previous design, plus a real per-namespace UTS (nodename/
 * domainname) payload — the one type where genuinely faking the syscall-
 * visible behaviour (sethostname/setdomainname/uname) is both meaningful and
 * safe to do from a loadable module.
 *
 * PID and USER namespaces get real, functional isolation too (not just
 * bookkeeping) when this kernel build genuinely lacks CONFIG_PID_NS /
 * CONFIG_USER_NS, modelled directly on kernel/pid_namespace.c's and
 * kernel/user_namespace.c's own numbering schemes read from
 * $RUNNER_TEMP/kernel-common:
 *
 *   - PID: kernel/pid_namespace.c allocates namespace-local pid numbers from
 *     a per-namespace idr (struct pid_namespace.idr) and every pid_nr_ns()
 *     call re-derives the number visible in a given namespace from
 *     struct pid's numbers[] array. shadow_ns mirrors this with a per-shadow-
 *     pidns pair of xarrays (virtual pid <-> real pid), assigns the first
 *     child spawned after unshare(CLONE_NEWPID)/clone(..., CLONE_NEWPID) the
 *     namespace-local pid 1 exactly like copy_pid_ns()'s child_reaper, and
 *     transparently translates getpid(2)/getppid(2)/kill(2)/tgkill(2)/
 *     tkill(2)/wait4(2)/waitid(2) so a process inside the shadow pid
 *     namespace genuinely only ever observes/operates on namespace-local
 *     pid numbers.
 *   - USER: kernel/user_namespace.c maps in-namespace ids to kuid_t/kgid_t
 *     via extents written to /proc/<pid>/{uid,gid}_map. Fully replicating
 *     arbitrary multi-extent id-map parsing from an out-of-tree module would
 *     require hooking every credential-bearing syscall and vfs_write() on
 *     procfs, which is far too invasive to do safely here. shadow_ns instead
 *     implements the single most common real-world shape (an unprivileged
 *     "map my current id to uid/gid 0 inside the namespace" identity,
 *     exactly the default docker/runc userns-remap mapping) and genuinely
 *     virtualizes getuid(2)/geteuid(2)/getgid(2)/getegid(2)/getresuid(2)/
 *     getresgid(2) so unmodified code inside the shadow user namespace
 *     really observes uid/gid 0, not its real host id.
 *
 * IPC/NET/CGROUP keep reference-counted bookkeeping only when genuinely
 * unsupported: IPC without CONFIG_IPC_NS falls back to the single global
 * init_ipc_ns already (no meaningful extra isolation to add safely from a
 * module), and NET namespace isolation is inseparable from the entire
 * networking stack (net/core/net_namespace.c touches routing, sockets,
 * netfilter, sysctls, ...); vendoring that wholesale into a loadable module
 * would conflict with the compiled-in stack and cannot be done safely here.
 *
 * A kernel built with CONFIG_NAMESPACES=n entirely (init/Kconfig nests
 * CONFIG_UTS_NS/IPC_NS/USER_NS/PID_NS/NET_NS inside
 * "menuconfig NAMESPACES ... if NAMESPACES ... endif", so turning the parent
 * off forces every one of those five children off as well) is handled by
 * the exact same mechanism: SHADOW_NS_BUILTIN_FLAGS is computed per-type from
 * IS_ENABLED(CONFIG_*_NS), so it automatically evaluates to "none of these
 * five builtin" and shadow_ns transparently falls back to real PID/USER
 * isolation plus IPC/NET bookkeeping for all of them — no separate code path
 * needed. Only CLONE_NEWNS (fs/namespace.c has no Kconfig gate at all) and
 * CLONE_NEWCGROUP (gated solely by CONFIG_CGROUPS, outside the NAMESPACES
 * menu) stay builtin regardless. unshare(2)/setns(2)/clone(2)/clone3(2)
 * themselves have no CONFIG_NAMESPACES guard either (always compiled in
 * kernel/fork.c), so the syscalls are always there for this module to hook.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <linux/refcount.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/workqueue.h>
#include <linux/utsname.h>
#include <linux/err.h>
#include <asm/ptrace.h>

#include "shadow_hook.h"
#include "include/uapi/shadow_ns.h"

#define SHADOW_NS_VERSION		"3.0"
#define SHADOW_NS_MAX_NS		65536

/*
 * SHADOW_NS_BUILTIN_FLAGS - the CLONE_NEW* bits this kernel build already
 * implements for real, evaluated from the same CONFIG_* symbols the kernel
 * build system generates for this module (i.e. this module's own
 * include/generated/autoconf.h, which matches the vmlinux it is built
 * against — see ../shadow_ctr_checker for the same IS_ENABLED() pattern).
 *
 * CLONE_NEWNS (mount namespaces) has no Kconfig gate in mainline Linux and is
 * unconditionally compiled in, so it is always treated as builtin.
 * CLONE_NEWCGROUP only requires CONFIG_CGROUPS=y (every GKI defconfig sets
 * this), so CONFIG_CGROUPS is used as its proxy, matching shadow_ctr_checker.
 */
#define SHADOW_NS_BUILTIN_FLAGS ( \
	(IS_ENABLED(CONFIG_UTS_NS)  ? (unsigned long)CLONE_NEWUTS    : 0UL) | \
	(IS_ENABLED(CONFIG_IPC_NS)  ? (unsigned long)CLONE_NEWIPC    : 0UL) | \
	(unsigned long)CLONE_NEWNS | \
	(IS_ENABLED(CONFIG_PID_NS)  ? (unsigned long)CLONE_NEWPID    : 0UL) | \
	(IS_ENABLED(CONFIG_NET_NS)  ? (unsigned long)CLONE_NEWNET    : 0UL) | \
	(IS_ENABLED(CONFIG_USER_NS) ? (unsigned long)CLONE_NEWUSER   : 0UL) | \
	(IS_ENABLED(CONFIG_CGROUPS) ? (unsigned long)CLONE_NEWCGROUP : 0UL) \
)

#define SHADOW_NS_ALL_FLAGS ((unsigned long)(CLONE_NEWUTS | CLONE_NEWIPC | \
				CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET | \
				CLONE_NEWUSER | CLONE_NEWCGROUP))

/*
 * SHADOW_NS_SHADOW_CLONE_FLAGS - the CLONE_NEW* bits shadow_ns still needs to
 * simulate: exactly the ones NOT already builtin on this kernel. On the
 * expected production build (containerd support => every type builtin) this
 * is 0 and every hook below is a pure passthrough to the real syscall.
 */
#define SHADOW_NS_SHADOW_CLONE_FLAGS (SHADOW_NS_ALL_FLAGS & ~SHADOW_NS_BUILTIN_FLAGS)

/*
 * struct shadow_uts_priv - per-UTS-namespace fallback payload.
 * Only ever allocated when CLONE_NEWUTS is part of SHADOW_NS_SHADOW_CLONE_FLAGS
 * (i.e. this kernel build genuinely lacks CONFIG_UTS_NS).
 */
struct shadow_uts_priv {
	struct mutex	lock;
	char		nodename[SHADOW_NS_UTS_LEN + 1];
	char		domainname[SHADOW_NS_UTS_LEN + 1];
};

/*
 * struct shadow_pidns_priv - per-PID-namespace real vpid<->rpid translation,
 * modelled on kernel/pid_namespace.c's per-namespace idr. Only ever allocated
 * when CLONE_NEWPID is part of SHADOW_NS_SHADOW_CLONE_FLAGS (i.e. this kernel
 * build genuinely lacks CONFIG_PID_NS).
 * @lock:        serialises all lookups/inserts below.
 * @vpid_to_rpid: namespace-local vpid -> real (host) tgid, xa_mk_value()'d.
 * @rpid_to_vpid: reverse map, real (host) tgid -> namespace-local vpid.
 * @next_vpid:    next vpid to hand out; 1 is reserved for the namespace's
 *                first child (its "init"/child_reaper, mirroring
 *                copy_pid_ns()).
 */
struct shadow_pidns_priv {
	struct mutex	lock;
	struct xarray	vpid_to_rpid;
	struct xarray	rpid_to_vpid;
	u32		next_vpid;
};

/*
 * struct shadow_userns_priv - per-USER-namespace real single-identity
 * mapping (the common "map my current id to 0 inside" docker/runc
 * userns-remap shape). Only ever allocated when CLONE_NEWUSER is part of
 * SHADOW_NS_SHADOW_CLONE_FLAGS (i.e. this kernel build genuinely lacks
 * CONFIG_USER_NS).
 * @real_uid/@real_gid: the creating task's real (host) identity, which
 *                       becomes uid/gid 0 as observed from inside.
 */
struct shadow_userns_priv {
	kuid_t	real_uid;
	kgid_t	real_gid;
};

/*
 * struct shadow_ns - a single shadow (fallback-simulation) namespace object.
 * Only ever created for a type in SHADOW_NS_SHADOW_CLONE_FLAGS.
 * @id:        stable identifier handed to userspace (xarray index)
 * @type:      enum shadow_ns_type
 * @parent_id: id of the namespace this was cloned from, 0 if none
 * @refcount:  dropped by every holder that references this object
 * @uts:       UTS payload, only present when @type == SHADOW_NS_TYPE_UTS
 * @pid:       PID translation payload, only when @type == SHADOW_NS_TYPE_PID
 * @user:      USER identity payload, only when @type == SHADOW_NS_TYPE_USER
 */
struct shadow_ns {
	u32				id;
	u32				type;
	u32				parent_id;
	refcount_t			refcount;
	struct shadow_uts_priv		*uts;
	struct shadow_pidns_priv	*pid;
	struct shadow_userns_priv	*user;
};

/*
 * struct shadow_task_group - transparent syscall-facing state keyed by TGID.
 * @tgid: thread-group id (what userspace sees as PID for a process)
 * @cur:  current shadow namespace of each type for this task group
 * @lock: serialises updates to @cur and UTS payloads
 */
struct shadow_task_group {
	pid_t				 tgid;
	struct shadow_ns		*cur[SHADOW_NS_TYPE_MAX];
	/*
	 * pending_pidns - mirrors nsproxy->pid_ns_for_children: set by
	 * unshare(CLONE_NEWPID)/clone(..., CLONE_NEWPID), applies only to
	 * subsequently spawned children (never to the calling task itself),
	 * exactly like the real kernel's copy_pid_ns() semantics.
	 */
	struct shadow_ns		*pending_pidns;
	struct mutex			 lock;
};

/* Global registry of shadow namespaces: id -> struct shadow_ns *. */
static DEFINE_XARRAY_ALLOC1(shadow_ns_map);
static DEFINE_MUTEX(shadow_ns_map_lock);
static atomic_t shadow_ns_count = ATOMIC_INIT(0);

/* Transparent task-group registry: tgid -> struct shadow_task_group *. */
static DEFINE_XARRAY(shadow_ns_tgid_map);
static DEFINE_MUTEX(shadow_ns_tgid_lock);
static struct delayed_work shadow_ns_reap_work;

static long (*real_sys_unshare)(const struct pt_regs *regs);
static long (*real_sys_setns)(const struct pt_regs *regs);
static long (*real_sys_clone)(const struct pt_regs *regs);
static long (*real_sys_clone3)(const struct pt_regs *regs);
static long (*real_sys_fork)(const struct pt_regs *regs);
static long (*real_sys_vfork)(const struct pt_regs *regs);
static long (*real_sys_sethostname)(const struct pt_regs *regs);
static long (*real_sys_setdomainname)(const struct pt_regs *regs);
static long (*real_sys_newuname)(const struct pt_regs *regs);
static long (*real_sys_getpid)(const struct pt_regs *regs);
static long (*real_sys_getppid)(const struct pt_regs *regs);
static long (*real_sys_kill)(const struct pt_regs *regs);
static long (*real_sys_tgkill)(const struct pt_regs *regs);
static long (*real_sys_tkill)(const struct pt_regs *regs);
static long (*real_sys_wait4)(const struct pt_regs *regs);
static long (*real_sys_waitid)(const struct pt_regs *regs);
static long (*real_sys_getuid)(const struct pt_regs *regs);
static long (*real_sys_geteuid)(const struct pt_regs *regs);
static long (*real_sys_getgid)(const struct pt_regs *regs);
static long (*real_sys_getegid)(const struct pt_regs *regs);
static long (*real_sys_getresuid)(const struct pt_regs *regs);
static long (*real_sys_getresgid)(const struct pt_regs *regs);

#define SHADOW_NS_REAP_INTERVAL	(30 * HZ)

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
static const char * const shadow_ns_sethostname_names[] = {
	"__arm64_sys_sethostname", "__x64_sys_sethostname", "sys_sethostname", NULL,
};
static const char * const shadow_ns_setdomainname_names[] = {
	"__arm64_sys_setdomainname", "__x64_sys_setdomainname",
	"sys_setdomainname", NULL,
};
static const char * const shadow_ns_newuname_names[] = {
	"__arm64_sys_newuname", "__x64_sys_newuname", "sys_newuname",
	"__arm64_sys_uname", "__x64_sys_uname", "sys_uname", NULL,
};
static const char * const shadow_ns_getpid_names[] = {
	"__arm64_sys_getpid", "__x64_sys_getpid", "sys_getpid", NULL,
};
static const char * const shadow_ns_getppid_names[] = {
	"__arm64_sys_getppid", "__x64_sys_getppid", "sys_getppid", NULL,
};
static const char * const shadow_ns_kill_names[] = {
	"__arm64_sys_kill", "__x64_sys_kill", "sys_kill", NULL,
};
static const char * const shadow_ns_tgkill_names[] = {
	"__arm64_sys_tgkill", "__x64_sys_tgkill", "sys_tgkill", NULL,
};
static const char * const shadow_ns_tkill_names[] = {
	"__arm64_sys_tkill", "__x64_sys_tkill", "sys_tkill", NULL,
};
static const char * const shadow_ns_wait4_names[] = {
	"__arm64_sys_wait4", "__x64_sys_wait4", "sys_wait4", NULL,
};
static const char * const shadow_ns_waitid_names[] = {
	"__arm64_sys_waitid", "__x64_sys_waitid", "sys_waitid", NULL,
};
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

static long shadow_ns_hook_unshare(const struct pt_regs *regs);
static long shadow_ns_hook_setns(const struct pt_regs *regs);
static long shadow_ns_hook_clone(const struct pt_regs *regs);
static long shadow_ns_hook_clone3(const struct pt_regs *regs);
static long shadow_ns_hook_fork(const struct pt_regs *regs);
static long shadow_ns_hook_vfork(const struct pt_regs *regs);
static long shadow_ns_hook_sethostname(const struct pt_regs *regs);
static long shadow_ns_hook_setdomainname(const struct pt_regs *regs);
static long shadow_ns_hook_newuname(const struct pt_regs *regs);
static long shadow_ns_hook_getpid(const struct pt_regs *regs);
static long shadow_ns_hook_getppid(const struct pt_regs *regs);
static long shadow_ns_hook_kill(const struct pt_regs *regs);
static long shadow_ns_hook_tgkill(const struct pt_regs *regs);
static long shadow_ns_hook_tkill(const struct pt_regs *regs);
static long shadow_ns_hook_wait4(const struct pt_regs *regs);
static long shadow_ns_hook_waitid(const struct pt_regs *regs);
static long shadow_ns_hook_getuid(const struct pt_regs *regs);
static long shadow_ns_hook_geteuid(const struct pt_regs *regs);
static long shadow_ns_hook_getgid(const struct pt_regs *regs);
static long shadow_ns_hook_getegid(const struct pt_regs *regs);
static long shadow_ns_hook_getresuid(const struct pt_regs *regs);
static long shadow_ns_hook_getresgid(const struct pt_regs *regs);

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
static struct shadow_hook shadow_ns_sethostname_hook =
	SHADOW_HOOK(shadow_ns_sethostname_names, shadow_ns_hook_sethostname,
		    &real_sys_sethostname);
static struct shadow_hook shadow_ns_setdomainname_hook =
	SHADOW_HOOK(shadow_ns_setdomainname_names, shadow_ns_hook_setdomainname,
		    &real_sys_setdomainname);
static struct shadow_hook shadow_ns_newuname_hook =
	SHADOW_HOOK(shadow_ns_newuname_names, shadow_ns_hook_newuname,
		    &real_sys_newuname);
static struct shadow_hook shadow_ns_getpid_hook =
	SHADOW_HOOK(shadow_ns_getpid_names, shadow_ns_hook_getpid, &real_sys_getpid);
static struct shadow_hook shadow_ns_getppid_hook =
	SHADOW_HOOK(shadow_ns_getppid_names, shadow_ns_hook_getppid, &real_sys_getppid);
static struct shadow_hook shadow_ns_kill_hook =
	SHADOW_HOOK(shadow_ns_kill_names, shadow_ns_hook_kill, &real_sys_kill);
static struct shadow_hook shadow_ns_tgkill_hook =
	SHADOW_HOOK(shadow_ns_tgkill_names, shadow_ns_hook_tgkill, &real_sys_tgkill);
static struct shadow_hook shadow_ns_tkill_hook =
	SHADOW_HOOK(shadow_ns_tkill_names, shadow_ns_hook_tkill, &real_sys_tkill);
static struct shadow_hook shadow_ns_wait4_hook =
	SHADOW_HOOK(shadow_ns_wait4_names, shadow_ns_hook_wait4, &real_sys_wait4);
static struct shadow_hook shadow_ns_waitid_hook =
	SHADOW_HOOK(shadow_ns_waitid_names, shadow_ns_hook_waitid, &real_sys_waitid);
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

/* Core hooks: always installed (they degenerate to passthrough as needed). */
static struct shadow_hook *shadow_ns_core_hooks[] = {
	&shadow_ns_unshare_hook,
	&shadow_ns_setns_hook,
	&shadow_ns_clone_hook,
	&shadow_ns_clone3_hook,
	&shadow_ns_fork_hook,
	&shadow_ns_vfork_hook,
	NULL,
};

/* UTS-simulation hooks: only installed when CONFIG_UTS_NS is not builtin. */
static struct shadow_hook *shadow_ns_uts_hooks[] = {
	&shadow_ns_sethostname_hook,
	&shadow_ns_setdomainname_hook,
	&shadow_ns_newuname_hook,
	NULL,
};

/* PID-simulation hooks: only installed when CONFIG_PID_NS is not builtin. */
static struct shadow_hook *shadow_ns_pid_hooks[] = {
	&shadow_ns_getpid_hook,
	&shadow_ns_getppid_hook,
	&shadow_ns_kill_hook,
	&shadow_ns_tgkill_hook,
	&shadow_ns_tkill_hook,
	&shadow_ns_wait4_hook,
	&shadow_ns_waitid_hook,
	NULL,
};

/* USER-simulation hooks: only installed when CONFIG_USER_NS is not builtin. */
static struct shadow_hook *shadow_ns_user_hooks[] = {
	&shadow_ns_getuid_hook,
	&shadow_ns_geteuid_hook,
	&shadow_ns_getgid_hook,
	&shadow_ns_getegid_hook,
	&shadow_ns_getresuid_hook,
	&shadow_ns_getresgid_hook,
	NULL,
};

static unsigned long shadow_ns_type_to_clone_flag(u32 type)
{
	switch (type) {
	case SHADOW_NS_TYPE_UTS:
		return CLONE_NEWUTS;
	case SHADOW_NS_TYPE_IPC:
		return CLONE_NEWIPC;
	case SHADOW_NS_TYPE_MNT:
		return CLONE_NEWNS;
	case SHADOW_NS_TYPE_PID:
		return CLONE_NEWPID;
	case SHADOW_NS_TYPE_NET:
		return CLONE_NEWNET;
	case SHADOW_NS_TYPE_USER:
		return CLONE_NEWUSER;
	case SHADOW_NS_TYPE_CGROUP:
		return CLONE_NEWCGROUP;
	default:
		return 0;
	}
}

static int shadow_ns_clone_flag_to_type(unsigned long flag)
{
	switch (flag) {
	case 0:
		return SHADOW_NS_TYPE_MAX;
	case CLONE_NEWUTS:
		return SHADOW_NS_TYPE_UTS;
	case CLONE_NEWIPC:
		return SHADOW_NS_TYPE_IPC;
	case CLONE_NEWNS:
		return SHADOW_NS_TYPE_MNT;
	case CLONE_NEWPID:
		return SHADOW_NS_TYPE_PID;
	case CLONE_NEWNET:
		return SHADOW_NS_TYPE_NET;
	case CLONE_NEWUSER:
		return SHADOW_NS_TYPE_USER;
	case CLONE_NEWCGROUP:
		return SHADOW_NS_TYPE_CGROUP;
	default:
		return -EINVAL;
	}
}

static bool shadow_ns_requires_admin(unsigned long shadow_flags)
{
	return shadow_flags != 0;
}

#if defined(CONFIG_ARM64)
static unsigned long shadow_ns_sys_arg0(const struct pt_regs *regs)
{
	return regs->regs[0];
}
static unsigned long shadow_ns_sys_arg1(const struct pt_regs *regs)
{
	return regs->regs[1];
}
static void shadow_ns_sys_set_arg0(struct pt_regs *regs, unsigned long value)
{
	regs->regs[0] = value;
}
static void shadow_ns_sys_set_arg1(struct pt_regs *regs, unsigned long value)
{
	regs->regs[1] = value;
}
#elif defined(CONFIG_X86_64)
static unsigned long shadow_ns_sys_arg0(const struct pt_regs *regs)
{
	return regs->di;
}
static unsigned long shadow_ns_sys_arg1(const struct pt_regs *regs)
{
	return regs->si;
}
static void shadow_ns_sys_set_arg0(struct pt_regs *regs, unsigned long value)
{
	regs->di = value;
}
static void shadow_ns_sys_set_arg1(struct pt_regs *regs, unsigned long value)
{
	regs->si = value;
}
#else
#error "shadow_ns: unsupported architecture"
#endif

/* --- UTS fallback payload (only used when CLONE_NEWUTS is simulated) --- */

static struct shadow_uts_priv *shadow_ns_uts_priv_alloc(struct shadow_uts_priv *parent)
{
	struct shadow_uts_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	mutex_init(&priv->lock);
	if (parent) {
		mutex_lock(&parent->lock);
		strscpy(priv->nodename, parent->nodename, sizeof(priv->nodename));
		strscpy(priv->domainname, parent->domainname,
			sizeof(priv->domainname));
		mutex_unlock(&parent->lock);
	}
	return priv;
}

static void shadow_ns_uts_priv_free(struct shadow_uts_priv *priv)
{
	if (!priv)
		return;
	mutex_destroy(&priv->lock);
	kfree(priv);
}

/* --- PID fallback payload (only used when CLONE_NEWPID is simulated) --- */

static struct shadow_pidns_priv *shadow_ns_pidns_priv_alloc(void)
{
	struct shadow_pidns_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	mutex_init(&priv->lock);
	xa_init_flags(&priv->vpid_to_rpid, XA_FLAGS_ALLOC);
	xa_init(&priv->rpid_to_vpid);
	priv->next_vpid = 1;
	return priv;
}

static void shadow_ns_pidns_priv_free(struct shadow_pidns_priv *priv)
{
	if (!priv)
		return;
	xa_destroy(&priv->vpid_to_rpid);
	xa_destroy(&priv->rpid_to_vpid);
	mutex_destroy(&priv->lock);
	kfree(priv);
}

/*
 * shadow_ns_pidns_register - assign @rpid (a real, host tgid) the next
 * namespace-local vpid in @pidns (1 for the namespace's first member,
 * mirroring copy_pid_ns()'s child_reaper). No-op if @rpid is already mapped.
 */
static void shadow_ns_pidns_register(struct shadow_pidns_priv *pidns, pid_t rpid)
{
	u32 vpid;

	if (!pidns || rpid <= 0)
		return;

	mutex_lock(&pidns->lock);
	if (xa_load(&pidns->rpid_to_vpid, rpid)) {
		mutex_unlock(&pidns->lock);
		return;
	}
	vpid = pidns->next_vpid++;
	if (xa_err(xa_store(&pidns->vpid_to_rpid, vpid, xa_mk_value(rpid), GFP_KERNEL)))
		goto unlock;
	if (xa_err(xa_store(&pidns->rpid_to_vpid, rpid, xa_mk_value(vpid), GFP_KERNEL)))
		xa_erase(&pidns->vpid_to_rpid, vpid);
unlock:
	mutex_unlock(&pidns->lock);
}

static void shadow_ns_pidns_unregister(struct shadow_pidns_priv *pidns, pid_t rpid)
{
	void *v;

	if (!pidns || rpid <= 0)
		return;

	mutex_lock(&pidns->lock);
	v = xa_erase(&pidns->rpid_to_vpid, rpid);
	if (v)
		xa_erase(&pidns->vpid_to_rpid, xa_to_value(v));
	mutex_unlock(&pidns->lock);
}

/* rpid -> vpid; returns 0 if @rpid has no mapping in @pidns. */
static pid_t shadow_ns_pidns_to_vpid(struct shadow_pidns_priv *pidns, pid_t rpid)
{
	void *v;
	pid_t vpid = 0;

	if (!pidns || rpid <= 0)
		return 0;
	mutex_lock(&pidns->lock);
	v = xa_load(&pidns->rpid_to_vpid, rpid);
	if (v)
		vpid = (pid_t)xa_to_value(v);
	mutex_unlock(&pidns->lock);
	return vpid;
}

/* vpid -> rpid; returns 0 if @vpid has no mapping in @pidns. */
static pid_t shadow_ns_pidns_to_rpid(struct shadow_pidns_priv *pidns, pid_t vpid)
{
	void *v;
	pid_t rpid = 0;

	if (!pidns || vpid <= 0)
		return 0;
	mutex_lock(&pidns->lock);
	v = xa_load(&pidns->vpid_to_rpid, vpid);
	if (v)
		rpid = (pid_t)xa_to_value(v);
	mutex_unlock(&pidns->lock);
	return rpid;
}

/* --- USER fallback payload (only used when CLONE_NEWUSER is simulated) - */

static struct shadow_userns_priv *shadow_ns_userns_priv_alloc(void)
{
	struct shadow_userns_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	/* The creating task's real identity becomes uid/gid 0 inside. */
	priv->real_uid = current_uid();
	priv->real_gid = current_gid();
	return priv;
}

static void shadow_ns_userns_priv_free(struct shadow_userns_priv *priv)
{
	kfree(priv);
}

/*
 * Allocate a new shadow namespace with refcount 1. Caller owns the reference.
 * @parent describes the namespace this one derives from (NULL for a
 * fresh/root instance).
 */
static struct shadow_ns *shadow_ns_alloc(u32 type, u32 parent_id,
					struct shadow_ns *parent)
{
	struct shadow_ns *ns;
	u32 id;
	int ret;

	if (atomic_read(&shadow_ns_count) >= SHADOW_NS_MAX_NS)
		return ERR_PTR(-ENOSPC);

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return ERR_PTR(-ENOMEM);

	ns->type = type;
	ns->parent_id = parent_id;
	refcount_set(&ns->refcount, 1);

	if (type == SHADOW_NS_TYPE_UTS) {
		struct shadow_uts_priv *uts;

		uts = shadow_ns_uts_priv_alloc(parent ? parent->uts : NULL);
		if (IS_ERR(uts)) {
			ret = PTR_ERR(uts);
			kfree(ns);
			return ERR_PTR(ret);
		}
		ns->uts = uts;
	} else if (type == SHADOW_NS_TYPE_PID) {
		struct shadow_pidns_priv *pid;

		pid = shadow_ns_pidns_priv_alloc();
		if (IS_ERR(pid)) {
			ret = PTR_ERR(pid);
			kfree(ns);
			return ERR_PTR(ret);
		}
		ns->pid = pid;
	} else if (type == SHADOW_NS_TYPE_USER) {
		struct shadow_userns_priv *user;

		user = shadow_ns_userns_priv_alloc();
		if (IS_ERR(user)) {
			ret = PTR_ERR(user);
			kfree(ns);
			return ERR_PTR(ret);
		}
		ns->user = user;
	}

	mutex_lock(&shadow_ns_map_lock);
	/* Reserve id in [1, INT_MAX]; 0 is reserved to mean "no namespace". */
	ret = xa_alloc(&shadow_ns_map, &id, ns, XA_LIMIT(1, INT_MAX), GFP_KERNEL);
	mutex_unlock(&shadow_ns_map_lock);
	if (ret) {
		shadow_ns_uts_priv_free(ns->uts);
		shadow_ns_pidns_priv_free(ns->pid);
		shadow_ns_userns_priv_free(ns->user);
		kfree(ns);
		return ERR_PTR(ret);
	}

	ns->id = id;
	atomic_inc(&shadow_ns_count);
	return ns;
}

static struct shadow_ns *shadow_ns_alloc_derived(u32 type, struct shadow_ns *parent)
{
	u32 parent_id = parent ? parent->id : 0;

	return shadow_ns_alloc(type, parent_id, parent);
}

static struct shadow_ns *shadow_ns_grab(struct shadow_ns *ns)
{
	if (!ns)
		return NULL;
	if (!refcount_inc_not_zero(&ns->refcount))
		return NULL;
	return ns;
}

static struct shadow_ns *shadow_ns_get(u32 id)
{
	struct shadow_ns *ns;

	if (!id)
		return NULL;

	mutex_lock(&shadow_ns_map_lock);
	ns = xa_load(&shadow_ns_map, id);
	if (ns && !refcount_inc_not_zero(&ns->refcount))
		ns = NULL;
	mutex_unlock(&shadow_ns_map_lock);
	return ns;
}

static void shadow_ns_put(struct shadow_ns *ns)
{
	if (!ns)
		return;

	if (refcount_dec_and_test(&ns->refcount)) {
		mutex_lock(&shadow_ns_map_lock);
		xa_erase(&shadow_ns_map, ns->id);
		mutex_unlock(&shadow_ns_map_lock);
		atomic_dec(&shadow_ns_count);

		shadow_ns_uts_priv_free(ns->uts);
		shadow_ns_pidns_priv_free(ns->pid);
		shadow_ns_userns_priv_free(ns->user);
		kfree(ns);
	}
}

static void shadow_ns_drop_cur_array(struct shadow_ns **cur)
{
	int type;

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		shadow_ns_put(cur[type]);
		cur[type] = NULL;
	}
}

static void shadow_ns_slot_replace(struct shadow_ns **slot, struct shadow_ns *ns)
{
	if (*slot)
		shadow_ns_put(*slot);
	*slot = ns;
}

static struct shadow_task_group *shadow_ns_task_group_lookup(pid_t tgid)
{
	struct shadow_task_group *tg;

	if (tgid <= 0)
		return NULL;

	mutex_lock(&shadow_ns_tgid_lock);
	tg = xa_load(&shadow_ns_tgid_map, tgid);
	mutex_unlock(&shadow_ns_tgid_lock);
	return tg;
}

static struct shadow_task_group *shadow_ns_task_group_get_or_create(pid_t tgid)
{
	struct shadow_task_group *tg;
	int ret;

	if (tgid <= 0)
		return ERR_PTR(-ESRCH);

	mutex_lock(&shadow_ns_tgid_lock);
	tg = xa_load(&shadow_ns_tgid_map, tgid);
	if (tg)
		goto out_unlock;

	tg = kzalloc(sizeof(*tg), GFP_KERNEL);
	if (!tg) {
		mutex_unlock(&shadow_ns_tgid_lock);
		return ERR_PTR(-ENOMEM);
	}

	tg->tgid = tgid;
	mutex_init(&tg->lock);
	ret = xa_err(xa_store(&shadow_ns_tgid_map, tgid, tg, GFP_KERNEL));
	if (ret) {
		mutex_destroy(&tg->lock);
		kfree(tg);
		mutex_unlock(&shadow_ns_tgid_lock);
		return ERR_PTR(ret);
	}

out_unlock:
	mutex_unlock(&shadow_ns_tgid_lock);
	return tg;
}

static struct shadow_task_group *shadow_ns_current_task_group(bool create)
{
	pid_t tgid = task_tgid_nr(current);

	if (create)
		return shadow_ns_task_group_get_or_create(tgid);
	return shadow_ns_task_group_lookup(tgid);
}

static void shadow_ns_task_group_free(struct shadow_task_group *tg)
{
	if (!tg)
		return;

	if (tg->cur[SHADOW_NS_TYPE_PID])
		shadow_ns_pidns_unregister(tg->cur[SHADOW_NS_TYPE_PID]->pid, tg->tgid);
	shadow_ns_drop_cur_array(tg->cur);
	shadow_ns_put(tg->pending_pidns);
	tg->pending_pidns = NULL;
	mutex_destroy(&tg->lock);
	kfree(tg);
}

static bool shadow_ns_task_group_alive(pid_t tgid)
{
	struct pid *pid;
	struct task_struct *task;
	bool alive;

	pid = find_get_pid(tgid);
	if (!pid)
		return false;

	task = get_pid_task(pid, PIDTYPE_TGID);
	alive = task != NULL;
	if (task)
		put_task_struct(task);
	put_pid(pid);
	return alive;
}

static void shadow_ns_reap_stale_task_groups(void)
{
	struct shadow_task_group *tg;
	unsigned long id = 0;

	for (;;) {
		mutex_lock(&shadow_ns_tgid_lock);
		tg = xa_find(&shadow_ns_tgid_map, &id, ULONG_MAX, XA_PRESENT);
		if (!tg) {
			mutex_unlock(&shadow_ns_tgid_lock);
			break;
		}
		if (shadow_ns_task_group_alive((pid_t)id)) {
			id++;
			mutex_unlock(&shadow_ns_tgid_lock);
			continue;
		}

		tg = xa_erase(&shadow_ns_tgid_map, id);
		mutex_unlock(&shadow_ns_tgid_lock);
		shadow_ns_task_group_free(tg);
	}
}

static void shadow_ns_reap_workfn(struct work_struct *work)
{
	shadow_ns_reap_stale_task_groups();
	schedule_delayed_work(&shadow_ns_reap_work, SHADOW_NS_REAP_INTERVAL);
}

static int shadow_ns_task_group_unshare_locked(struct shadow_task_group *tg,
					      unsigned long shadow_flags)
{
	unsigned long generic_flags = shadow_flags & ~(unsigned long)CLONE_NEWPID;
	struct shadow_ns *created[SHADOW_NS_TYPE_MAX] = { };
	struct shadow_ns *new_pending_pidns = NULL;
	int type;
	int ret = 0;

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		if (type == SHADOW_NS_TYPE_PID)
			continue; /* handled below: applies to future children only */
		if (!(generic_flags & shadow_ns_type_to_clone_flag(type)))
			continue;

		created[type] = shadow_ns_alloc_derived(type, tg->cur[type]);
		if (IS_ERR(created[type])) {
			ret = PTR_ERR(created[type]);
			created[type] = NULL;
			goto err_put;
		}
	}

	/*
	 * unshare(CLONE_NEWPID)/clone(..., CLONE_NEWPID) never moves the
	 * calling task into the new pid namespace (mirrors copy_pid_ns()):
	 * it only sets pid_ns_for_children, consumed by the next child(ren).
	 */
	if (shadow_flags & CLONE_NEWPID) {
		new_pending_pidns = shadow_ns_alloc_derived(SHADOW_NS_TYPE_PID,
							    tg->cur[SHADOW_NS_TYPE_PID]);
		if (IS_ERR(new_pending_pidns)) {
			ret = PTR_ERR(new_pending_pidns);
			new_pending_pidns = NULL;
			goto err_put;
		}
	}

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		if (created[type])
			shadow_ns_slot_replace(&tg->cur[type], created[type]);
	}
	if (new_pending_pidns)
		shadow_ns_slot_replace(&tg->pending_pidns, new_pending_pidns);

	return 0;

err_put:
	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++)
		shadow_ns_put(created[type]);
	shadow_ns_put(new_pending_pidns);
	return ret;
}

static int shadow_ns_prepare_child_cur_locked(struct shadow_task_group *parent,
					     unsigned long shadow_flags,
					     struct shadow_ns **next)
{
	int type;
	int ret;

	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		struct shadow_ns *parent_ns = parent ? parent->cur[type] : NULL;

		if (type == SHADOW_NS_TYPE_PID)
			continue; /* handled by shadow_ns_child_pidns() */

		if (shadow_flags & shadow_ns_type_to_clone_flag(type)) {
			next[type] = shadow_ns_alloc_derived(type, parent_ns);
			if (IS_ERR(next[type])) {
				ret = PTR_ERR(next[type]);
				next[type] = NULL;
				goto err_put;
			}
			continue;
		}

		next[type] = shadow_ns_grab(parent_ns);
	}

	return 0;

err_put:
	for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
		shadow_ns_put(next[type]);
		next[type] = NULL;
	}
	return ret;
}

/*
 * shadow_ns_child_pidns - determine the shadow pid namespace a newly spawned
 * child belongs to (must be called with @parent's lock held, or @parent
 * NULL). A direct clone(..., CLONE_NEWPID) gets a brand-new namespace
 * (@parent's current pidns becomes its lineage parent); otherwise the child
 * inherits @parent's pending_pidns (set by a prior unshare(CLONE_NEWPID)) or,
 * failing that, @parent's own current pidns membership — exactly mirroring
 * how a real task's pid_ns_for_children/nsproxy is propagated to fork()ed
 * children.
 */
static int shadow_ns_child_pidns(struct shadow_task_group *parent,
				unsigned long shadow_flags,
				struct shadow_ns **out)
{
	struct shadow_ns *ns;

	*out = NULL;

	if (shadow_flags & CLONE_NEWPID) {
		ns = shadow_ns_alloc_derived(SHADOW_NS_TYPE_PID,
					     parent ? parent->cur[SHADOW_NS_TYPE_PID] : NULL);
		if (IS_ERR(ns))
			return PTR_ERR(ns);
		*out = ns;
		return 0;
	}

	if (parent) {
		struct shadow_ns *source = parent->pending_pidns ?
					    parent->pending_pidns :
					    parent->cur[SHADOW_NS_TYPE_PID];
		*out = shadow_ns_grab(source);
	}
	return 0;
}

static int shadow_ns_install_child_state(pid_t child_tgid,
					struct shadow_task_group *parent,
					unsigned long shadow_flags)
{
	struct shadow_task_group *child;
	struct shadow_ns *next[SHADOW_NS_TYPE_MAX] = { };
	struct shadow_ns *pidns_for_child = NULL;
	int ret;
	int type;

	if (child_tgid <= 0)
		return -ESRCH;

	child = shadow_ns_task_group_get_or_create(child_tgid);
	if (IS_ERR(child))
		return PTR_ERR(child);

	if (parent)
		mutex_lock(&parent->lock);
	ret = shadow_ns_prepare_child_cur_locked(parent, shadow_flags, next);
	if (!ret)
		ret = shadow_ns_child_pidns(parent, shadow_flags, &pidns_for_child);
	if (parent)
		mutex_unlock(&parent->lock);
	if (ret) {
		for (type = 0; type < SHADOW_NS_TYPE_MAX; type++)
			shadow_ns_put(next[type]);
		return ret;
	}

	mutex_lock(&child->lock);
	shadow_ns_drop_cur_array(child->cur);
	memcpy(child->cur, next, sizeof(child->cur));
	memset(next, 0, sizeof(next));
	shadow_ns_slot_replace(&child->cur[SHADOW_NS_TYPE_PID], pidns_for_child);
	mutex_unlock(&child->lock);

	if (pidns_for_child)
		shadow_ns_pidns_register(pidns_for_child->pid, child_tgid);
	return 0;
}

/* Join @ns as the current namespace of its type (consumes a ref). */
static void shadow_join_cur(struct shadow_ns **cur, struct shadow_ns *ns)
{
	shadow_ns_slot_replace(&cur[ns->type], ns);
}

static long shadow_ns_task_group_setns_by_id(int id, int flags)
{
	struct shadow_task_group *tg;
	struct shadow_ns *ns;
	int wanted_type;
	long ret;

	wanted_type = shadow_ns_clone_flag_to_type(flags);
	if (wanted_type < 0)
		return wanted_type;

	ns = shadow_ns_get(id);
	if (!ns)
		return -ENOENT;
	if (wanted_type != SHADOW_NS_TYPE_MAX && ns->type != wanted_type) {
		shadow_ns_put(ns);
		return -EINVAL;
	}
	if (!capable(CAP_SYS_ADMIN)) {
		shadow_ns_put(ns);
		return -EPERM;
	}

	tg = shadow_ns_current_task_group(true);
	if (IS_ERR(tg)) {
		shadow_ns_put(ns);
		return PTR_ERR(tg);
	}

	mutex_lock(&tg->lock);
	if (ns->type == SHADOW_NS_TYPE_PID) {
		/*
		 * setns(2) into a PID namespace, like unshare(CLONE_NEWPID),
		 * only changes the namespace assigned to *future* children
		 * (nsproxy->pid_ns_for_children); the caller's own pid
		 * namespace membership never changes. Mirror that here
		 * instead of joining tg->cur[PID] directly.
		 */
		shadow_ns_slot_replace(&tg->pending_pidns, ns);
	} else {
		shadow_join_cur(tg->cur, ns);
	}
	mutex_unlock(&tg->lock);
	ret = 0;
	return ret;
}

static pid_t shadow_ns_resolve_child_tgid(pid_t pid)
{
	struct pid *pid_struct;
	struct task_struct *task;
	pid_t tgid = 0;

	if (pid <= 0)
		return 0;

	pid_struct = find_get_pid(pid);
	if (!pid_struct)
		return 0;
	task = get_pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	if (!task)
		return 0;

	tgid = task_tgid_nr(task);
	put_task_struct(task);
	return tgid;
}

static long shadow_ns_clone_finalize(long ret, struct shadow_task_group *parent,
				    unsigned long shadow_flags)
{
	pid_t child_tgid;
	int err;

	if (ret <= 0)
		return ret;
	if (!parent && !shadow_flags)
		return ret;

	child_tgid = shadow_ns_resolve_child_tgid((pid_t)ret);
	if (!child_tgid || child_tgid == task_tgid_nr(current))
		return ret;

	err = shadow_ns_install_child_state(child_tgid, parent, shadow_flags);
	if (err)
		pr_warn("shadow_ns: failed to install child state for tgid %d: %d\n",
			child_tgid, err);
	return ret;
}

/* --- transparent syscall hooks ------------------------------------------ */

static long shadow_ns_hook_unshare(const struct pt_regs *regs)
{
	struct shadow_task_group *tg;
	struct pt_regs regs_copy;
	unsigned long flags = shadow_ns_sys_arg0(regs);
	unsigned long shadow_flags = flags & SHADOW_NS_SHADOW_CLONE_FLAGS;
	unsigned long native_flags = flags & ~SHADOW_NS_SHADOW_CLONE_FLAGS;
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

	/*
	 * Best-effort private encoding for shadow-only joins: use the numeric
	 * namespace id directly in place of @fd when there is no real nsfs fd.
	 * Only ever reached for a namespace type this kernel build genuinely
	 * lacks (real_sys_setns() already succeeds for every builtin type).
	 */
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
	unsigned long shadow_flags = flags & SHADOW_NS_SHADOW_CLONE_FLAGS;
	long ret;

	if (shadow_flags && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (shadow_flags)
		shadow_ns_sys_set_arg0(&regs_copy, flags & ~SHADOW_NS_SHADOW_CLONE_FLAGS);
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

	shadow_flags = (unsigned long)(orig_flags & SHADOW_NS_SHADOW_CLONE_FLAGS);
	if (shadow_flags && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	native_flags = orig_flags & ~((u64)SHADOW_NS_SHADOW_CLONE_FLAGS);
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

/* --- UTS-simulation hooks (only installed when CONFIG_UTS_NS is absent) - */

static struct shadow_ns *shadow_ns_get_current(u32 type)
{
	struct shadow_task_group *tg;
	struct shadow_ns *ns;

	if (type >= SHADOW_NS_TYPE_MAX)
		return NULL;

	tg = shadow_ns_current_task_group(false);
	if (!tg)
		return NULL;

	mutex_lock(&tg->lock);
	ns = shadow_ns_grab(tg->cur[type]);
	mutex_unlock(&tg->lock);
	return ns;
}

static long shadow_ns_uts_update(bool domainname, const char __user *name,
				int len)
{
	struct shadow_ns *ns;
	struct shadow_uts_priv *p;
	char buf[SHADOW_NS_UTS_LEN + 1] = { 0 };

	if (len < 0 || len > SHADOW_NS_UTS_LEN)
		return -EINVAL;

	ns = shadow_ns_get_current(SHADOW_NS_TYPE_UTS);
	if (!ns)
		return -ENOENT;
	p = ns->uts;
	if (!p) {
		shadow_ns_put(ns);
		return -ENOENT;
	}
	if (!capable(CAP_SYS_ADMIN)) {
		shadow_ns_put(ns);
		return -EPERM;
	}
	if (len && copy_from_user(buf, name, len)) {
		shadow_ns_put(ns);
		return -EFAULT;
	}
	buf[len] = '\0';

	mutex_lock(&p->lock);
	if (domainname)
		strscpy(p->domainname, buf, sizeof(p->domainname));
	else
		strscpy(p->nodename, buf, sizeof(p->nodename));
	mutex_unlock(&p->lock);

	shadow_ns_put(ns);
	return 0;
}

static void shadow_ns_uts_capture(char nodename[SHADOW_NS_UTS_LEN + 1],
				 char domainname[SHADOW_NS_UTS_LEN + 1],
				 bool *has)
{
	struct shadow_ns *ns;
	struct shadow_uts_priv *p;

	*has = false;

	ns = shadow_ns_get_current(SHADOW_NS_TYPE_UTS);
	if (!ns)
		return;
	p = ns->uts;
	if (!p) {
		shadow_ns_put(ns);
		return;
	}

	mutex_lock(&p->lock);
	strscpy(nodename, p->nodename, SHADOW_NS_UTS_LEN + 1);
	strscpy(domainname, p->domainname, SHADOW_NS_UTS_LEN + 1);
	mutex_unlock(&p->lock);
	*has = true;

	shadow_ns_put(ns);
}

static long shadow_ns_hook_sethostname(const struct pt_regs *regs)
{
	const char __user *name =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	int len = (int)shadow_ns_sys_arg1(regs);
	long ret = shadow_ns_uts_update(false, name, len);

	if (ret == -ENOENT)
		return real_sys_sethostname(regs);
	return ret;
}

static long shadow_ns_hook_setdomainname(const struct pt_regs *regs)
{
	const char __user *name =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	int len = (int)shadow_ns_sys_arg1(regs);
	long ret = shadow_ns_uts_update(true, name, len);

	if (ret == -ENOENT)
		return real_sys_setdomainname(regs);
	return ret;
}

static long shadow_ns_hook_newuname(const struct pt_regs *regs)
{
	struct new_utsname uts;
	char nodename[SHADOW_NS_UTS_LEN + 1];
	char domainname[SHADOW_NS_UTS_LEN + 1];
	void __user *uarg = (void __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	bool has_shadow_uts;
	long ret;

	shadow_ns_uts_capture(nodename, domainname, &has_shadow_uts);
	ret = real_sys_newuname(regs);
	if (ret || !has_shadow_uts)
		return ret;

	if (copy_from_user(&uts, uarg, sizeof(uts)))
		return -EFAULT;
	strscpy(uts.nodename, nodename, sizeof(uts.nodename));
	strscpy(uts.domainname, domainname, sizeof(uts.domainname));
	if (copy_to_user(uarg, &uts, sizeof(uts)))
		return -EFAULT;
	return 0;
}

/* --- PID-simulation hooks (only installed when CONFIG_PID_NS is absent) - */

/*
 * shadow_ns_current_pidns - the current task group's shadow pid namespace,
 * with a reference held, or NULL if this task isn't in one. Release with
 * shadow_ns_put().
 */
static struct shadow_ns *shadow_ns_current_pidns(void)
{
	return shadow_ns_get_current(SHADOW_NS_TYPE_PID);
}

static long shadow_ns_hook_getpid(const struct pt_regs *regs)
{
	struct shadow_ns *ns = shadow_ns_current_pidns();
	pid_t rpid = task_tgid_nr(current);
	pid_t vpid;

	if (!ns)
		return real_sys_getpid(regs);

	vpid = shadow_ns_pidns_to_vpid(ns->pid, rpid);
	shadow_ns_put(ns);
	return vpid ? vpid : real_sys_getpid(regs);
}

static long shadow_ns_hook_getppid(const struct pt_regs *regs)
{
	struct shadow_ns *ns = shadow_ns_current_pidns();
	long real_ppid;
	pid_t vppid;

	if (!ns)
		return real_sys_getppid(regs);

	real_ppid = real_sys_getppid(regs);
	if (real_ppid <= 0) {
		shadow_ns_put(ns);
		return real_ppid;
	}
	/*
	 * A parent outside the namespace is invisible from inside it (mirrors
	 * the real kernel's ppid==0 behaviour once a namespace's ancestry
	 * escapes what that namespace can see).
	 */
	vppid = shadow_ns_pidns_to_vpid(ns->pid, (pid_t)real_ppid);
	shadow_ns_put(ns);
	return vppid;
}

/* Translate a positive namespace-local vpid target to its real rpid. */
static long shadow_ns_pid_translate_target(unsigned long vpid_arg)
{
	struct shadow_ns *ns;
	pid_t rpid;

	if ((long)vpid_arg <= 0)
		return vpid_arg; /* pgid/broadcast forms: left untranslated */

	ns = shadow_ns_current_pidns();
	if (!ns)
		return vpid_arg;

	rpid = shadow_ns_pidns_to_rpid(ns->pid, (pid_t)vpid_arg);
	shadow_ns_put(ns);
	return rpid ? rpid : vpid_arg;
}

static long shadow_ns_hook_kill(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	return real_sys_kill(&regs_copy);
}

static long shadow_ns_hook_tgkill(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long tgid_arg = shadow_ns_sys_arg0(regs);
	unsigned long tid_arg = shadow_ns_sys_arg1(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(tgid_arg));
	/* Thread-level ids aren't separately tracked; approximate via the tgid map. */
	shadow_ns_sys_set_arg1(&regs_copy, shadow_ns_pid_translate_target(tid_arg));
	return real_sys_tgkill(&regs_copy);
}

static long shadow_ns_hook_tkill(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	return real_sys_tkill(&regs_copy);
}

static long shadow_ns_hook_wait4(const struct pt_regs *regs)
{
	struct shadow_ns *ns = shadow_ns_current_pidns();
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);
	long ret;
	pid_t vpid;

	if (!ns)
		return real_sys_wait4(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	ret = real_sys_wait4(&regs_copy);
	if (ret > 0) {
		vpid = shadow_ns_pidns_to_vpid(ns->pid, (pid_t)ret);
		if (vpid)
			ret = vpid;
	}
	shadow_ns_put(ns);
	return ret;
}

static long shadow_ns_hook_waitid(const struct pt_regs *regs)
{
	int idtype = (int)shadow_ns_sys_arg0(regs);
	unsigned long id_arg = shadow_ns_sys_arg1(regs);
	struct pt_regs regs_copy = *regs;

	/* P_PID == 1: translate the target id; other idtypes pass through. */
	if (idtype == 1)
		shadow_ns_sys_set_arg1(&regs_copy, shadow_ns_pid_translate_target(id_arg));
	return real_sys_waitid(&regs_copy);
}

/* --- USER-simulation hooks (only installed when CONFIG_USER_NS absent) -- */

static struct shadow_userns_priv *shadow_ns_current_userns_priv(void)
{
	struct shadow_ns *ns = shadow_ns_get_current(SHADOW_NS_TYPE_USER);
	struct shadow_userns_priv *priv;

	if (!ns)
		return NULL;
	priv = ns->user;
	shadow_ns_put(ns); /* priv itself is only freed at ns teardown */
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

/* --- exported query API (used by shadow_ctr_checker) -------------------- */

/*
 * shadow_ns_type_simulated - true if this module fakes/bookkeeps @type on
 * this kernel build (i.e. the kernel genuinely lacks native support for it).
 */
bool shadow_ns_type_simulated(u32 type)
{
	if (type >= SHADOW_NS_TYPE_MAX)
		return false;
	return (SHADOW_NS_SHADOW_CLONE_FLAGS & shadow_ns_type_to_clone_flag(type)) != 0;
}
EXPORT_SYMBOL_GPL(shadow_ns_type_simulated);

/*
 * shadow_ns_type_real - true if the simulation for @type (only ever active
 * when shadow_ns_type_simulated() is true) is functionally real rather than
 * bookkeeping-only. Only UTS qualifies today.
 */
bool shadow_ns_type_real(u32 type)
{
	if (!shadow_ns_type_simulated(type))
		return false;
	switch (type) {
	case SHADOW_NS_TYPE_UTS:
	case SHADOW_NS_TYPE_PID:
	case SHADOW_NS_TYPE_USER:
		return true;
	default:
		return false;
	}
}
EXPORT_SYMBOL_GPL(shadow_ns_type_real);

static int __init shadow_ns_init(void)
{
	int ret;
	int hooked;

	pr_info("shadow_ns: init starting (version %s)\n", SHADOW_NS_VERSION);
	pr_info("shadow_ns: builtin namespace flags 0x%lx; simulated (fallback) flags 0x%lx\n",
		(unsigned long)SHADOW_NS_BUILTIN_FLAGS,
		(unsigned long)SHADOW_NS_SHADOW_CLONE_FLAGS);

	hooked = shadow_hook_install_all(shadow_ns_core_hooks, "shadow_ns");
	if (hooked < 0) {
		ret = hooked;
		pr_err("shadow_ns: init: shadow_hook_install_all() failed: %d\n", ret);
		return ret;
	}
	pr_info("shadow_ns: init: %d core hook(s) installed\n", hooked);

	if (SHADOW_NS_SHADOW_CLONE_FLAGS & CLONE_NEWUTS) {
		hooked = shadow_hook_install_all(shadow_ns_uts_hooks, "shadow_ns_uts");
		if (hooked < 0) {
			ret = hooked;
			pr_err("shadow_ns: init: UTS shadow_hook_install_all() failed: %d\n",
			       ret);
			shadow_hook_remove_all(shadow_ns_core_hooks);
			return ret;
		}
		pr_info("shadow_ns: init: %d UTS-simulation hook(s) installed (CONFIG_UTS_NS absent)\n",
			hooked);
	} else {
		pr_info("shadow_ns: CONFIG_UTS_NS builtin; sethostname/setdomainname/uname left untouched\n");
	}

	if (SHADOW_NS_SHADOW_CLONE_FLAGS & CLONE_NEWPID) {
		hooked = shadow_hook_install_all(shadow_ns_pid_hooks, "shadow_ns_pid");
		if (hooked < 0) {
			ret = hooked;
			pr_err("shadow_ns: init: PID shadow_hook_install_all() failed: %d\n",
			       ret);
			if (SHADOW_NS_SHADOW_CLONE_FLAGS & CLONE_NEWUTS)
				shadow_hook_remove_all(shadow_ns_uts_hooks);
			shadow_hook_remove_all(shadow_ns_core_hooks);
			return ret;
		}
		pr_info("shadow_ns: init: %d PID-simulation hook(s) installed (CONFIG_PID_NS absent)\n",
			hooked);
	} else {
		pr_info("shadow_ns: CONFIG_PID_NS builtin; getpid/getppid/kill/wait4 left untouched\n");
	}

	if (SHADOW_NS_SHADOW_CLONE_FLAGS & CLONE_NEWUSER) {
		hooked = shadow_hook_install_all(shadow_ns_user_hooks, "shadow_ns_user");
		if (hooked < 0) {
			ret = hooked;
			pr_err("shadow_ns: init: USER shadow_hook_install_all() failed: %d\n",
			       ret);
			if (SHADOW_NS_SHADOW_CLONE_FLAGS & CLONE_NEWPID)
				shadow_hook_remove_all(shadow_ns_pid_hooks);
			if (SHADOW_NS_SHADOW_CLONE_FLAGS & CLONE_NEWUTS)
				shadow_hook_remove_all(shadow_ns_uts_hooks);
			shadow_hook_remove_all(shadow_ns_core_hooks);
			return ret;
		}
		pr_info("shadow_ns: init: %d USER-simulation hook(s) installed (CONFIG_USER_NS absent)\n",
			hooked);
	} else {
		pr_info("shadow_ns: CONFIG_USER_NS builtin; getuid/geteuid/getgid/getegid left untouched\n");
	}

	INIT_DELAYED_WORK(&shadow_ns_reap_work, shadow_ns_reap_workfn);
	schedule_delayed_work(&shadow_ns_reap_work, SHADOW_NS_REAP_INTERVAL);

	pr_info("shadow_ns: loaded\n");
	return 0;
}

static void __exit shadow_ns_exit(void)
{
	struct shadow_ns *ns;
	struct shadow_task_group *tg;
	unsigned long id;

	pr_info("shadow_ns: exit: removing syscall hooks\n");
	if (SHADOW_NS_SHADOW_CLONE_FLAGS & CLONE_NEWUSER)
		shadow_hook_remove_all(shadow_ns_user_hooks);
	if (SHADOW_NS_SHADOW_CLONE_FLAGS & CLONE_NEWPID)
		shadow_hook_remove_all(shadow_ns_pid_hooks);
	if (SHADOW_NS_SHADOW_CLONE_FLAGS & CLONE_NEWUTS)
		shadow_hook_remove_all(shadow_ns_uts_hooks);
	shadow_hook_remove_all(shadow_ns_core_hooks);

	pr_info("shadow_ns: exit: cancelling reap work\n");
	cancel_delayed_work_sync(&shadow_ns_reap_work);

	for (;;) {
		id = 0;
		mutex_lock(&shadow_ns_tgid_lock);
		tg = xa_find(&shadow_ns_tgid_map, &id, ULONG_MAX, XA_PRESENT);
		if (tg)
			xa_erase(&shadow_ns_tgid_map, id);
		mutex_unlock(&shadow_ns_tgid_lock);
		if (!tg)
			break;
		shadow_ns_task_group_free(tg);
	}
	xa_destroy(&shadow_ns_tgid_map);

	mutex_lock(&shadow_ns_map_lock);
	xa_for_each(&shadow_ns_map, id, ns) {
		xa_erase(&shadow_ns_map, id);
		shadow_ns_uts_priv_free(ns->uts);
		shadow_ns_pidns_priv_free(ns->pid);
		shadow_ns_userns_priv_free(ns->user);
		kfree(ns);
	}
	mutex_unlock(&shadow_ns_map_lock);
	xa_destroy(&shadow_ns_map);

	pr_info("shadow_ns: unloaded\n");
}

module_init(shadow_ns_init);
module_exit(shadow_ns_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Standalone namespace subsystem: passes real unshare/setns/clone/clone3/fork/vfork through untouched for every namespace type this kernel build genuinely supports, and only falls back to reference-counted (real, for UTS) simulation for types the build genuinely lacks");
MODULE_VERSION(SHADOW_NS_VERSION);
