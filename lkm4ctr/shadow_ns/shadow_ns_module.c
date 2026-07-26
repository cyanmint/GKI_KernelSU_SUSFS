// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns - standalone namespace subsystem
 *
 * The lkm4ctr.ko namespace subsystem intercepts unshare(2), setns(2),
 * clone(2)/clone3(2)/fork(2)/vfork(2) so an unmodified
 * `containerd`/`runc`/`dockerd` gets full namespace isolation.
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
 * SHADOW_NS_BUILTIN_FLAGS_COMPILETIME below). Every namespace flag bit the kernel really
 * supports is left completely untouched in the arguments passed to the real
 * unshare()/setns()/clone()/clone3() syscalls, so the kernel's own real,
 * fully-conformant namespace subsystem does the actual isolation work, with
 * shadow_ns getting out of the way entirely (see shadow_ns_clone_flags
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
 * NET/CGROUP/MNT keep reference-counted bookkeeping only when genuinely
 * unsupported: NET namespace isolation is inseparable from the entire
 * networking stack (net/core/net_namespace.c touches routing, sockets,
 * netfilter, sysctls, ...); vendoring that wholesale into a loadable module
 * would conflict with the compiled-in stack and cannot be done safely here.
 * IPC without CONFIG_IPC_NS gets real functional isolation for its SysV IPC
 * data path instead: shadow_ns still only tracks a plain id/refcount
 * bookkeeping object here (this module has no dedicated IPC-object storage
 * of its own -- see shadow_ns_alloc() below), but ../shadow_sysvipc/'s
 * transparent msgget/semget/shmget/... syscall hooks consult
 * shadow_ns_current_ipc_ns_id() and, whenever it is non-zero, always route
 * the calling task through a namespace-scoped shadow SysV IPC registry
 * instead of the real, un-partitioned init_ipc_ns -- turning shadow_ns's
 * id/refcount bookkeeping into the namespace key the two subsystems (linked
 * into the same lkm4ctr.ko) share to keep simulated IPC namespaces' SysV
 * objects genuinely isolated from each other and from the host. POSIX
 * message queues are not covered by this and remain bookkeeping-only.
 * CGROUP is included here too: on a kernel built with CONFIG_CGROUPS=n (rare
 * -- every GKI defconfig sets it, but not guaranteed by any other Kconfig
 * relationship), CLONE_NEWCGROUP drops out of SHADOW_NS_BUILTIN_FLAGS_COMPILETIME and
 * shadow_ns transparently falls back to the exact same generic, no-special-
 * payload bookkeeping object (id/parent/refcount only, see shadow_ns_alloc())
 * used for NET -- no separate code path is needed, since the alloc/clone/
 * unshare/setns machinery below is already fully generic over "type".
 *
 * MNT (CLONE_NEWNS) is the odd one out: fs/namespace.c/kernel/nsproxy.c have
 * no dedicated per-type Kconfig gate at all in mainline Linux (verified
 * against $RUNNER_TEMP/kernel-common: every CLONE_NEWNS check in
 * kernel/nsproxy.c and kernel/fork.c is unconditional, not
 * "#ifdef CONFIG_NAMESPACES"), so there is no CONFIG_MNT_NS symbol to key
 * off of the way UTS/IPC/USER/PID/NET each have their own. This module uses
 * CONFIG_NAMESPACES itself (the parent menuconfig, "default !EXPERT" i.e.
 * effectively always on) as MNT's builtin gate instead, purely as a
 * defensive fallback: on CONFIG_NAMESPACES=n, CLONE_NEWNS drops out of
 * SHADOW_NS_BUILTIN_FLAGS_COMPILETIME and MNT gets the exact same bookkeeping-only
 * treatment as IPC/NET/CGROUP above. This does not disable or replace the
 * real, always-compiled-in mount-namespace machinery in fs/namespace.c
 * (which keeps running regardless of CONFIG_NAMESPACES); it only means
 * shadow_ns additionally tracks a bookkeeping id/refcount entry for it, for
 * parity with every other type when this config combination is hit.
 *
 * A kernel built with CONFIG_NAMESPACES=n entirely (init/Kconfig nests
 * CONFIG_UTS_NS/IPC_NS/USER_NS/PID_NS/NET_NS inside
 * "menuconfig NAMESPACES ... if NAMESPACES ... endif", so turning the parent
 * off forces every one of those five children off as well) is handled by
 * the exact same mechanism: SHADOW_NS_BUILTIN_FLAGS_COMPILETIME is computed per-type from
 * IS_ENABLED(CONFIG_*_NS), so it automatically evaluates to "none of these
 * five builtin" and shadow_ns transparently falls back to real PID/USER
 * isolation plus real IPC (SysV) plus NET bookkeeping for all of them — no
 * separate code path needed. MNT (gated on CONFIG_NAMESPACES itself, as
 * described above) and CGROUP (gated solely by CONFIG_CGROUPS, also outside
 * the NAMESPACES menu) get the same bookkeeping fallback if their respective
 * config is off.
 * unshare(2)/setns(2)/clone(2)/clone3(2) themselves have no CONFIG_NAMESPACES
 * guard either (always compiled in kernel/fork.c), so the syscalls are
 * always there for this module to hook.
 */
#include "shadow_ns_internal.h"
#include "lkm4ctr_log.h"

unsigned long shadow_ns_clone_flags;

/*
 * shadow_ns_compute_clone_flags - determine, from the kernel actually
 * running (not the one this module was compiled against), which CLONE_NEW*
 * namespace types genuinely need shadow_ns's own simulation.
 *
 * Rationale / bug this fixes: this module is typically compiled once by a
 * fixed-KMI DDK container matching the production/containerd-enabled
 * kernel build (CONFIG_UTS_NS=y, CONFIG_IPC_NS=y, CONFIG_PID_NS=y,
 * CONFIG_USER_NS=y, CONFIG_NET_NS=y — see the file header), but the
 * resulting .ko can equally be loaded into a stock/unmodified GKI kernel
 * that lacks some of those. Deciding what to simulate purely from this
 * module's own compile-time IS_ENABLED(CONFIG_*_NS) (i.e.
 * SHADOW_NS_BUILTIN_FLAGS_COMPILETIME) silently assumes build-time and
 * run-time configs always match; when they don't (e.g. CONFIG_IPC_NS=y at
 * build time but absent on the stock kernel actually booted), a namespace
 * type gets wrongly left "untouched" as builtin, so the real syscall fails
 * (e.g. unshare(CLONE_NEWIPC) => -EINVAL) and shadow_ns's fallback never
 * kicks in.
 *
 * Fixed here by re-checking, at module load time, whether each namespace
 * type's own kernel object is actually linked into the *running* vmlinux —
 * via shadow_hook_resolve() (kallsyms-based lookup, safe regardless of
 * EXPORT_SYMBOL/CONFIG_TRIM_UNUSED_KSYMS) of a canary symbol that only
 * exists when the corresponding obj-$(CONFIG_*_NS) object is compiled in:
 *
 *   CONFIG_UTS_NS  -> copy_utsname()   (kernel/utsname.c,        obj-$(CONFIG_UTS_NS)  += utsname.o,        kernel/Makefile)
 *   CONFIG_IPC_NS  -> copy_ipcs()      (ipc/namespace.c,         obj-$(CONFIG_IPC_NS)  += namespace.o,      ipc/Makefile)
 *   CONFIG_PID_NS  -> copy_pid_ns()    (kernel/pid_namespace.c,  obj-$(CONFIG_PID_NS)  += pid_namespace.o,  kernel/Makefile)
 *   CONFIG_USER_NS -> create_user_ns() (kernel/user_namespace.c, obj-$(CONFIG_USER_NS) += user_namespace.o, kernel/Makefile)
 *   CONFIG_NET_NS  -> copy_net_ns()    (net/core/net_namespace.c)
 *
 * MNT (CLONE_NEWNS) and CGROUP (CLONE_NEWCGROUP) are left as compile-time
 * checks (CONFIG_NAMESPACES / CONFIG_CGROUPS respectively): both are
 * effectively always-on for every kernel this module targets and neither
 * has been observed to differ between build-time and run-time the way
 * IPC/UTS/PID/USER/NET did, so no canary lookup is needed for them.
 */
unsigned long shadow_ns_compute_clone_flags(void)
{
	unsigned long builtin = 0;

	if (shadow_hook_resolve("copy_utsname"))
		builtin |= CLONE_NEWUTS;
	if (shadow_hook_resolve("copy_ipcs"))
		builtin |= CLONE_NEWIPC;
	if (shadow_hook_resolve("copy_pid_ns"))
		builtin |= CLONE_NEWPID;
	if (shadow_hook_resolve("create_user_ns"))
		builtin |= CLONE_NEWUSER;
	if (shadow_hook_resolve("copy_net_ns"))
		builtin |= CLONE_NEWNET;
	if (IS_ENABLED(CONFIG_NAMESPACES))
		builtin |= CLONE_NEWNS;
	if (IS_ENABLED(CONFIG_CGROUPS))
		builtin |= CLONE_NEWCGROUP;

	return SHADOW_NS_ALL_FLAGS & ~builtin;
}

bool shadow_ns_type_simulated(u32 type)
{
	if (type >= SHADOW_NS_TYPE_MAX)
		return false;
	return (shadow_ns_clone_flags & shadow_ns_type_to_clone_flag(type)) != 0;
}
EXPORT_SYMBOL_GPL(shadow_ns_type_simulated);

bool shadow_ns_type_real(u32 type)
{
	if (!shadow_ns_type_simulated(type))
		return false;
	switch (type) {
	case SHADOW_NS_TYPE_UTS:
	case SHADOW_NS_TYPE_PID:
	case SHADOW_NS_TYPE_USER:
	case SHADOW_NS_TYPE_IPC:
		return true;
	default:
		return false;
	}
}
EXPORT_SYMBOL_GPL(shadow_ns_type_real);

int shadow_ns_init(void)
{
	int ret;
	int hooked;

	LKM4CTR_INFO("shadow_ns", "init starting (version %s)", SHADOW_NS_VERSION);

	shadow_ns_clone_flags = shadow_ns_compute_clone_flags();

	LKM4CTR_INFO("shadow_ns", "builtin namespace flags 0x%lx (compiled against 0x%lx); simulated (fallback) flags 0x%lx",
		     SHADOW_NS_ALL_FLAGS & ~shadow_ns_clone_flags,
		     (unsigned long)SHADOW_NS_BUILTIN_FLAGS_COMPILETIME, (unsigned long)shadow_ns_clone_flags);

	hooked = shadow_hook_install_all(shadow_ns_core_hooks, "shadow_ns");
	if (hooked < 0) {
		ret = hooked;
		LKM4CTR_ERR("shadow_ns", "init: shadow_hook_install_all() failed: %d", ret);
		return ret;
	}
	LKM4CTR_INFO("shadow_ns", "init: %d core hook(s) installed", hooked);

	if (shadow_ns_clone_flags & CLONE_NEWUTS) {
		hooked = shadow_hook_install_all(shadow_ns_uts_hooks, "shadow_ns_uts");
		if (hooked < 0) {
			ret = hooked;
			LKM4CTR_ERR("shadow_ns", "init: UTS shadow_hook_install_all() failed: %d", ret);
			shadow_hook_remove_all(shadow_ns_core_hooks);
			return ret;
		}
		LKM4CTR_INFO("shadow_ns", "init: %d UTS-simulation hook(s) installed (CONFIG_UTS_NS absent)",
			     hooked);
	} else {
		LKM4CTR_INFO("shadow_ns",
			     "CONFIG_UTS_NS builtin; sethostname/setdomainname/uname left untouched");
	}

	if (shadow_ns_clone_flags & CLONE_NEWPID) {
		hooked = shadow_hook_install_all(shadow_ns_pid_hooks, "shadow_ns_pid");
		if (hooked < 0) {
			ret = hooked;
			LKM4CTR_ERR("shadow_ns", "init: PID shadow_hook_install_all() failed: %d", ret);
			if (shadow_ns_clone_flags & CLONE_NEWUTS)
				shadow_hook_remove_all(shadow_ns_uts_hooks);
			shadow_hook_remove_all(shadow_ns_core_hooks);
			return ret;
		}
		LKM4CTR_INFO("shadow_ns", "init: %d PID-simulation hook(s) installed (CONFIG_PID_NS absent)",
			     hooked);
	} else {
		LKM4CTR_INFO("shadow_ns", "CONFIG_PID_NS builtin; getpid/getppid/kill/wait4 left untouched");
	}

	if (shadow_ns_clone_flags & CLONE_NEWUSER) {
		hooked = shadow_hook_install_all(shadow_ns_user_hooks, "shadow_ns_user");
		if (hooked < 0) {
			ret = hooked;
			LKM4CTR_ERR("shadow_ns", "init: USER shadow_hook_install_all() failed: %d", ret);
			if (shadow_ns_clone_flags & CLONE_NEWPID)
				shadow_hook_remove_all(shadow_ns_pid_hooks);
			if (shadow_ns_clone_flags & CLONE_NEWUTS)
				shadow_hook_remove_all(shadow_ns_uts_hooks);
			shadow_hook_remove_all(shadow_ns_core_hooks);
			return ret;
		}
		LKM4CTR_INFO("shadow_ns", "init: %d USER-simulation hook(s) installed (CONFIG_USER_NS absent)",
			     hooked);
	} else {
		LKM4CTR_INFO("shadow_ns", "CONFIG_USER_NS builtin; getuid/geteuid/getgid/getegid left untouched");
	}

	/*
	 * shadow_ns_procfs_hooks (openat2/openat/open/getdents64) serves
	 * several independent needs: fabricating /proc/<pid>/setgroups (only
	 * meaningful when CLONE_NEWUSER is simulated), translating/
	 * filtering /proc for a simulated PID namespace's own vpid<->rpid
	 * mapping (only meaningful when CLONE_NEWPID is simulated), and
	 * fabricating /proc/<pid>/ns/{user,ipc} (meaningful whenever USER,
	 * PID or IPC is simulated -- IPC's own ns entry matters both for its
	 * own now-real SysV IPC isolation (see shadow_sysvipc/) and because
	 * runc/containerd's namespace-support probe stats every ns/ entry as
	 * one combined check before issuing unshare()/clone3(): if ns/ipc
	 * looks absent, the whole combined namespace setup is aborted,
	 * silently discarding PID/USER simulation too). Install it whenever
	 * any of the three is active; each hook internally no-ops the parts
	 * of its logic that don't apply.
	 */
	if (shadow_ns_clone_flags & (CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWIPC)) {
		hooked = shadow_hook_install_all(shadow_ns_procfs_hooks, "shadow_ns_procfs");
		if (hooked < 0) {
			ret = hooked;
			LKM4CTR_ERR("shadow_ns", "init: procfs shadow_hook_install_all() failed: %d", ret);
			if (shadow_ns_clone_flags & CLONE_NEWUSER)
				shadow_hook_remove_all(shadow_ns_user_hooks);
			if (shadow_ns_clone_flags & CLONE_NEWPID)
				shadow_hook_remove_all(shadow_ns_pid_hooks);
			if (shadow_ns_clone_flags & CLONE_NEWUTS)
				shadow_hook_remove_all(shadow_ns_uts_hooks);
			shadow_hook_remove_all(shadow_ns_core_hooks);
			return ret;
		}
		LKM4CTR_INFO("shadow_ns", "init: %d procfs hook(s) installed (fabricating /proc/*/setgroups and/or isolating /proc for a simulated PID namespace)",
			     hooked);
	}

	INIT_DELAYED_WORK(&shadow_ns_reap_work, shadow_ns_reap_workfn);
	schedule_delayed_work(&shadow_ns_reap_work, SHADOW_NS_REAP_INTERVAL);

	LKM4CTR_INFO("shadow_ns", "loaded");
	return 0;
}

void shadow_ns_exit(void)
{
	struct shadow_ns *ns;
	struct shadow_task_group *tg;
	unsigned long id;

	LKM4CTR_INFO("shadow_ns", "exit: removing syscall hooks");
	if (shadow_ns_clone_flags & (CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWIPC))
		shadow_hook_remove_all(shadow_ns_procfs_hooks);
	if (shadow_ns_clone_flags & CLONE_NEWUSER)
		shadow_hook_remove_all(shadow_ns_user_hooks);
	if (shadow_ns_clone_flags & CLONE_NEWPID)
		shadow_hook_remove_all(shadow_ns_pid_hooks);
	if (shadow_ns_clone_flags & CLONE_NEWUTS)
		shadow_hook_remove_all(shadow_ns_uts_hooks);
	shadow_hook_remove_all(shadow_ns_core_hooks);

	LKM4CTR_INFO("shadow_ns", "exit: cancelling reap work");
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
		shadow_ns_task_group_put(tg);
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

	LKM4CTR_INFO("shadow_ns", "unloaded");
}
