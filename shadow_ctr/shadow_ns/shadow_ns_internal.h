// SPDX-License-Identifier: GPL-2.0
#ifndef SHADOW_NS_INTERNAL_H
#define SHADOW_NS_INTERNAL_H

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
 * SHADOW_NS_BUILTIN_FLAGS_COMPILETIME - the CLONE_NEW* bits this module was
 * *compiled* against, evaluated from the same CONFIG_* symbols the kernel
 * build system generates for this module (i.e. this module's own
 * include/generated/autoconf.h, which matches the vmlinux it is built
 * against — see ../shadow_ctr_checker for the same IS_ENABLED() pattern).
 *
 * IMPORTANT: this is only ever used as the *compile-time* half of the real,
 * runtime-computed shadow_ns_clone_flags (see shadow_ns_module.c's
 * shadow_ns_compute_clone_flags()), never on its own to decide what to
 * simulate. The module this file's build produces is routinely loaded into
 * a kernel whose actual running .config differs from the one it was built
 * against (e.g. this module family is built once via a fixed-KMI DDK
 * container matching the production/containerd-enabled kernel, but is also
 * tested against, and may be loaded into, a stock/unmodified GKI kernel that
 * lacks CONFIG_IPC_NS/CONFIG_POSIX_MQUEUE-derived namespace support). Using
 * IS_ENABLED(CONFIG_IPC_NS) alone to decide whether to simulate CLONE_NEWIPC
 * previously caused a real bug: it wrongly assumed IPC namespaces were
 * builtin (matching the DDK build config) and left unshare(CLONE_NEWIPC)
 * completely untouched, so on a stock kernel that genuinely lacks
 * CONFIG_IPC_NS the real syscall failed with -EINVAL and shadow_ns never
 * fell back to its own simulation. shadow_ns_compute_clone_flags() fixes
 * this by re-checking each type's real availability *at module load time*,
 * against the kernel actually running, via shadow_hook_resolve() lookups of
 * a canary symbol that only exists when the corresponding obj-$(CONFIG_*_NS)
 * kernel object is actually linked into the running vmlinux (see
 * shadow_ns_module.c for the exact symbol list and citations).
 *
 * CLONE_NEWNS (mount namespaces) has no dedicated per-type Kconfig gate
 * anywhere in mainline Linux (unlike UTS/IPC/USER/PID/NET, which each have
 * their own CONFIG_*_NS symbol nested inside "if NAMESPACES ... endif" in
 * init/Kconfig -- verified directly against $RUNNER_TEMP/kernel-common's
 * init/Kconfig and kernel/nsproxy.c, where every CLONE_NEWNS check is
 * unconditional, not "#ifdef CONFIG_NAMESPACES"). So CONFIG_NAMESPACES
 * itself (the parent menuconfig, not a child symbol) is used here as MNT's
 * own builtin gate: on every kernel this module actually targets it is
 * `default !EXPERT`, i.e. effectively always on, so this is a defensive
 * fallback for a hypothetical/non-standard build with CONFIG_NAMESPACES=n
 * rather than something expected to ever trigger in practice. If it is off,
 * MNT drops out of the builtin set and gets exactly the same
 * reference-counted bookkeeping-only fallback as IPC/NET/CGROUP below (the
 * alloc/unshare/clone/setns machinery is already fully generic over "type",
 * so no MNT-specific payload/code path is needed) -- it does NOT mean the
 * real, compiled-in mount-namespace machinery in fs/namespace.c stops
 * working; it means shadow_ns additionally maintains its own bookkeeping
 * registry entry for it, same as it would for IPC/NET/CGROUP. MNT and
 * CGROUP are left compile-time-only (no runtime canary) since both their
 * gating symbols (CONFIG_NAMESPACES, CONFIG_CGROUPS) are effectively always
 * on for every kernel this module targets and neither has been observed to
 * differ between the build-time and run-time kernel the way IPC/PID/USER
 * did.
 * CLONE_NEWCGROUP only requires CONFIG_CGROUPS=y (every GKI defconfig sets
 * this), so CONFIG_CGROUPS is used as its proxy, matching shadow_ctr_checker.
 */
#define SHADOW_NS_BUILTIN_FLAGS_COMPILETIME ( \
	(IS_ENABLED(CONFIG_UTS_NS)  ? (unsigned long)CLONE_NEWUTS    : 0UL) | \
	(IS_ENABLED(CONFIG_IPC_NS)  ? (unsigned long)CLONE_NEWIPC    : 0UL) | \
	(IS_ENABLED(CONFIG_NAMESPACES) ? (unsigned long)CLONE_NEWNS : 0UL) | \
	(IS_ENABLED(CONFIG_PID_NS)  ? (unsigned long)CLONE_NEWPID    : 0UL) | \
	(IS_ENABLED(CONFIG_NET_NS)  ? (unsigned long)CLONE_NEWNET    : 0UL) | \
	(IS_ENABLED(CONFIG_USER_NS) ? (unsigned long)CLONE_NEWUSER   : 0UL) | \
	(IS_ENABLED(CONFIG_CGROUPS) ? (unsigned long)CLONE_NEWCGROUP : 0UL) \
)

#define SHADOW_NS_ALL_FLAGS ((unsigned long)(CLONE_NEWUTS | CLONE_NEWIPC | \
				CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET | \
				CLONE_NEWUSER | CLONE_NEWCGROUP))

/*
 * shadow_ns_clone_flags - the CLONE_NEW* bits shadow_ns still needs to
 * simulate on *this* running kernel: exactly the ones NOT actually
 * available at runtime (see shadow_ns_compute_clone_flags(), called once
 * from shadow_ns_init() before any hook is installed). On the expected
 * production build/deploy combination (containerd support => every type
 * builtin on both ends) this is 0 and every hook below is a pure
 * passthrough to the real syscall.
 */
extern unsigned long shadow_ns_clone_flags;
unsigned long shadow_ns_compute_clone_flags(void);
#define SHADOW_NS_REAP_INTERVAL	(30 * HZ)

struct shadow_uts_priv {
	struct mutex	lock;
	char		nodename[SHADOW_NS_UTS_LEN + 1];
	char		domainname[SHADOW_NS_UTS_LEN + 1];
};

struct shadow_pidns_priv {
	struct mutex	lock;
	struct xarray	vpid_to_rpid;
	struct xarray	rpid_to_vpid;
	u32		next_vpid;
};

struct shadow_userns_priv {
	kuid_t	real_uid;
	kgid_t	real_gid;
};

struct shadow_ns {
	u32				id;
	u32				type;
	u32				parent_id;
	refcount_t			refcount;
	struct shadow_uts_priv		*uts;
	struct shadow_pidns_priv	*pid;
	struct shadow_userns_priv	*user;
};

struct shadow_task_group {
	pid_t				 tgid;
	struct shadow_ns		*cur[SHADOW_NS_TYPE_MAX];
	struct shadow_ns		*pending_pidns;
	struct mutex			 lock;
};

extern struct xarray shadow_ns_map;
extern struct mutex shadow_ns_map_lock;
extern atomic_t shadow_ns_count;
extern struct xarray shadow_ns_tgid_map;
extern struct mutex shadow_ns_tgid_lock;
extern struct delayed_work shadow_ns_reap_work;

unsigned long shadow_ns_type_to_clone_flag(u32 type);
int shadow_ns_clone_flag_to_type(unsigned long flag);
bool shadow_ns_requires_admin(unsigned long shadow_flags);

unsigned long shadow_ns_sys_arg0(const struct pt_regs *regs);
unsigned long shadow_ns_sys_arg1(const struct pt_regs *regs);
void shadow_ns_sys_set_arg0(struct pt_regs *regs, unsigned long value);
void shadow_ns_sys_set_arg1(struct pt_regs *regs, unsigned long value);

struct shadow_uts_priv *shadow_ns_uts_priv_alloc(struct shadow_uts_priv *parent);
void shadow_ns_uts_priv_free(struct shadow_uts_priv *priv);
struct shadow_pidns_priv *shadow_ns_pidns_priv_alloc(void);
void shadow_ns_pidns_priv_free(struct shadow_pidns_priv *priv);
void shadow_ns_pidns_register(struct shadow_pidns_priv *pidns, pid_t rpid);
void shadow_ns_pidns_unregister(struct shadow_pidns_priv *pidns, pid_t rpid);
pid_t shadow_ns_pidns_to_vpid(struct shadow_pidns_priv *pidns, pid_t rpid);
pid_t shadow_ns_pidns_to_rpid(struct shadow_pidns_priv *pidns, pid_t vpid);
struct shadow_userns_priv *shadow_ns_userns_priv_alloc(void);
void shadow_ns_userns_priv_free(struct shadow_userns_priv *priv);

struct shadow_ns *shadow_ns_alloc(u32 type, u32 parent_id, struct shadow_ns *parent);
struct shadow_ns *shadow_ns_alloc_derived(u32 type, struct shadow_ns *parent);
struct shadow_ns *shadow_ns_grab(struct shadow_ns *ns);
struct shadow_ns *shadow_ns_get(u32 id);
void shadow_ns_put(struct shadow_ns *ns);
void shadow_ns_drop_cur_array(struct shadow_ns **cur);
void shadow_ns_slot_replace(struct shadow_ns **slot, struct shadow_ns *ns);
struct shadow_ns *shadow_ns_get_current(u32 type);

struct shadow_task_group *shadow_ns_task_group_lookup(pid_t tgid);
struct shadow_task_group *shadow_ns_task_group_get_or_create(pid_t tgid);
struct shadow_task_group *shadow_ns_current_task_group(bool create);
void shadow_ns_task_group_free(struct shadow_task_group *tg);
bool shadow_ns_task_group_alive(pid_t tgid);
void shadow_ns_reap_stale_task_groups(void);
void shadow_ns_reap_workfn(struct work_struct *work);
int shadow_ns_task_group_unshare_locked(struct shadow_task_group *tg,
					unsigned long shadow_flags);
int shadow_ns_prepare_child_cur_locked(struct shadow_task_group *parent,
				      unsigned long shadow_flags,
				      struct shadow_ns **next);
int shadow_ns_child_pidns(struct shadow_task_group *parent,
			 unsigned long shadow_flags,
			 struct shadow_ns **out);
int shadow_ns_install_child_state(pid_t child_tgid,
				 struct shadow_task_group *parent,
				 unsigned long shadow_flags);
long shadow_ns_task_group_setns_by_id(int id, int flags);
pid_t shadow_ns_resolve_child_tgid(pid_t pid);
long shadow_ns_clone_finalize(long ret, struct shadow_task_group *parent,
			      unsigned long shadow_flags);

extern struct shadow_hook *shadow_ns_core_hooks[];
extern struct shadow_hook *shadow_ns_uts_hooks[];
extern struct shadow_hook *shadow_ns_pid_hooks[];
extern struct shadow_hook *shadow_ns_user_hooks[];

#endif
