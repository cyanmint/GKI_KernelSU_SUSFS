/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vendor_ns - internal declarations shared by the vendor_ns translation units.
 *
 * vendor_ns vendors the kernel's own namespace subsystem: each core algorithm
 * (ns_common inum allocation, uid/gid extent maps, per-namespace pid idr
 * allocation, nsproxy shape, UTS payload, and the generic reference-counted
 * namespace bookkeeping) is adapted directly from the corresponding upstream
 * Linux kernel source file, with every symbol renamed under a vns_/vendor_ns_
 * prefix so the vendored copies coexist with the real builtin kernel symbols
 * of the same name (this module hooks unconditionally, so both run at once).
 * The per-file credit headers name the exact kernel source each core is
 * vendored from.
 *
 * This header is intentionally never installed as UAPI; the userspace-visible
 * constants live in include/uapi/vendor_ns.h.
 */
#ifndef _VENDOR_NS_INTERNAL_H
#define _VENDOR_NS_INTERNAL_H

#include <linux/types.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/uidgid.h>
#include <linux/sched.h>

#include "include/uapi/vendor_ns.h"

#define VENDOR_NS_VERSION	"1.0-vendored"
#define VENDOR_NS_TAG		"vendor_ns"

/*
 * CLONE_NEWTIME (0x00000080) overlaps CSIGNAL in the legacy clone(2) ABI and is
 * only accepted via unshare(2)/clone3(2); provide a fallback for headers that
 * predate it so VENDOR_NS_ALL_FLAGS is always well-defined.
 */
#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME	0x00000080
#endif

/*
 * Every CLONE_NEW* bit vendor_ns is responsible for, unconditionally. Unlike
 * shadow_ns (whose flag mask is whittled down to only the types the running
 * kernel lacks), this set is fixed: vendor_ns always tracks all of them.
 */
#define VENDOR_NS_ALL_FLAGS \
	(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWPID | \
	 CLONE_NEWNET | CLONE_NEWUSER | CLONE_NEWCGROUP | CLONE_NEWTIME)

/* ------------------------------------------------------------------ */
/* Vendored core structures							*/
/* ------------------------------------------------------------------ */

/*
 * struct vns_ns_common - vendored from include/linux/ns_common.h.
 *
 * The kernel embeds a struct ns_common in every namespace object to carry the
 * namespace inode number (as shown in /proc/<pid>/ns/<type>) and a refcount.
 * We keep the same two fields plus a list linkage and type tag so the diagfs
 * layer can enumerate every live vendored namespace.
 */
struct vns_ns_common {
	unsigned int		inum;
	refcount_t		count;
	u32			type;		/* enum vendor_ns_type */
	struct list_head	registry;	/* vns_registry.ns_list[type] */
};

/* ---- UTS (vendored from kernel/utsname.c + include/linux/utsname.h) ---- */

#define VNS_UTS_LEN	64

/* Vendored from struct new_utsname (include/linux/utsname.h). */
struct vns_new_utsname {
	char sysname[VNS_UTS_LEN + 1];
	char nodename[VNS_UTS_LEN + 1];
	char release[VNS_UTS_LEN + 1];
	char version[VNS_UTS_LEN + 1];
	char machine[VNS_UTS_LEN + 1];
	char domainname[VNS_UTS_LEN + 1];
};

/* Vendored from struct uts_namespace (include/linux/utsname.h). */
struct vns_uts_namespace {
	struct vns_new_utsname	name;
	struct vns_ns_common	ns;
};

/* ---- USER (vendored from kernel/user_namespace.c) ---- */

#define VNS_UID_GID_MAP_MAX_EXTENTS	5

/* Vendored from struct uid_gid_extent (include/linux/user_namespace.h). */
struct vns_uid_gid_extent {
	u32 first;
	u32 lower_first;
	u32 count;
};

/*
 * Vendored from struct uid_gid_map (include/linux/user_namespace.h). Only the
 * small "base" extent array fast path is vendored (kernel supports up to 340
 * extents via a separately-allocated sorted array; a module needs only the
 * handful of extents a container runtime writes).
 */
struct vns_uid_gid_map {
	u32				nr_extents;
	struct vns_uid_gid_extent	extent[VNS_UID_GID_MAP_MAX_EXTENTS];
};

/* Vendored from struct user_namespace (include/linux/user_namespace.h). */
struct vns_user_namespace {
	struct vns_uid_gid_map		uid_map;
	struct vns_uid_gid_map		gid_map;
	struct vns_user_namespace	*parent;
	int				level;
	kuid_t				owner;
	kgid_t				group;
	struct vns_ns_common		ns;
};

/* ---- PID (vendored from kernel/pid.c + kernel/pid_namespace.c) ---- */

#define VNS_RESERVED_PIDS	300
#define VNS_PID_MAX_DEFAULT	0x8000

/* PIDNS_ADDING mirrors kernel/pid_namespace.c's pid_allocated bit. */
#define VNS_PIDNS_ADDING	0x80000000U

/* Vendored from struct pid_namespace (include/linux/pid_namespace.h, Linux 6.1.124). */
struct vns_pid_namespace {
	struct idr			idr;
	spinlock_t			lock;		/* protects idr + cursor */
	int				level;
	unsigned int			pid_allocated;
	struct vns_pid_namespace	*parent;
	struct vns_user_namespace	*user_ns;
	struct vns_ns_common		ns;
};

/* ---- generic bookkeeping namespaces (ipc/mnt/net/cgroup/time) ---- */

/*
 * struct vns_generic_namespace - reference-counted bookkeeping namespace for
 * the types vendor_ns tracks but does not fully functionally vendor from a
 * loadable module (IPC/MNT/CGROUP/NET/TIME). Modelled on the common shape of
 * ipc/namespace.c, fs/namespace.c, kernel/cgroup/namespace.c,
 * net/core/net_namespace.c and kernel/time/namespace.c, each of which is just
 * a payload plus an embedded struct ns_common.
 */
struct vns_generic_namespace {
	struct vns_ns_common	ns;
};

/* ---- nsproxy (vendored from kernel/nsproxy.c) ---- */

/* Vendored from struct nsproxy (include/linux/nsproxy.h). */
struct vns_nsproxy {
	refcount_t			count;
	struct vns_uts_namespace	*uts_ns;
	struct vns_generic_namespace	*ipc_ns;
	struct vns_generic_namespace	*mnt_ns;
	struct vns_pid_namespace	*pid_ns_for_children;
	struct vns_generic_namespace	*net_ns;
	struct vns_generic_namespace	*time_ns;
	struct vns_generic_namespace	*time_ns_for_children;
	struct vns_generic_namespace	*cgroup_ns;
	struct vns_user_namespace	*user_ns;
};

/*
 * struct vns_task - per thread-group membership record.
 *
 * A loadable module cannot rewrite task_struct->nsproxy, so vendor_ns keeps its
 * own side table mapping each tracked thread group (tgid) to the vendored
 * nsproxy it currently observes. This is the module-side analogue of
 * task_struct->nsproxy.
 */
struct vns_task {
	pid_t			tgid;
	struct vns_nsproxy	*nsproxy;
	struct hlist_node	node;
};

/* ------------------------------------------------------------------ */
/* Global registry						*/
/* ------------------------------------------------------------------ */

#define VNS_TASK_HASH_BITS	10

/*
 * struct vns_registry - single global holder of all vendored namespace state.
 */
struct vns_registry {
	struct mutex		lock;

	/* One list of live struct vns_ns_common per namespace type. */
	struct list_head	ns_list[VENDOR_NS_TYPE_MAX];
	unsigned long		ns_count[VENDOR_NS_TYPE_MAX];
	unsigned long		ns_created[VENDOR_NS_TYPE_MAX];

	/* Cyclic inum allocator (vendored idea from fs/nsfs.c proc_alloc_inum). */
	unsigned int		next_inum;

	/* Root vendored namespaces (the host's "namespace 1" per type). */
	struct vns_uts_namespace	*root_uts;
	struct vns_user_namespace	*root_user;
	struct vns_pid_namespace	*root_pid;
	struct vns_generic_namespace	*root_generic[VENDOR_NS_TYPE_MAX];
	struct vns_nsproxy		*root_nsproxy;

	/* Per-thread-group membership table. */
	DECLARE_HASHTABLE(tasks, VNS_TASK_HASH_BITS);
	unsigned long		task_count;

	/* Aggregate syscall counters, surfaced via diagfs. */
	unsigned long		stat_unshare;
	unsigned long		stat_setns;
	unsigned long		stat_clone;
	unsigned long		stat_uts_set;
	unsigned long		stat_uts_get;
	unsigned long		stat_pid_xlate;
	unsigned long		stat_uid_xlate;
	unsigned long		stat_proc_filtered;
};

extern struct vns_registry vendor_ns_registry;

/* ------------------------------------------------------------------ */
/* vns_nsfs.c - ns_common inum allocation + registry helpers	    */
/* ------------------------------------------------------------------ */

unsigned int vns_alloc_inum(void);
void vns_ns_common_init(struct vns_ns_common *ns, u32 type);
void vns_ns_register(struct vns_ns_common *ns);
void vns_ns_unregister(struct vns_ns_common *ns);
const char *vns_type_name(u32 type);

/* ------------------------------------------------------------------ */
/* vns_utsns.c - UTS namespace						*/
/* ------------------------------------------------------------------ */

struct vns_uts_namespace *vns_uts_root(void);
struct vns_uts_namespace *vns_clone_uts_ns(struct vns_uts_namespace *old);
void vns_free_uts_ns(struct vns_uts_namespace *ns);

/* ------------------------------------------------------------------ */
/* vns_userns.c - USER namespace + uid/gid maps			*/
/* ------------------------------------------------------------------ */

u32 vns_map_id_down(struct vns_uid_gid_map *map, u32 id);
u32 vns_map_id_up(struct vns_uid_gid_map *map, u32 id);
struct vns_user_namespace *vns_user_root(void);
struct vns_user_namespace *vns_create_user_ns(struct vns_user_namespace *parent);
void vns_free_user_ns(struct vns_user_namespace *ns);

/* ------------------------------------------------------------------ */
/* vns_pidns.c - PID namespace						*/
/* ------------------------------------------------------------------ */

struct vns_pid_namespace *vns_pid_root(void);
struct vns_pid_namespace *vns_create_pid_ns(struct vns_pid_namespace *parent,
					    struct vns_user_namespace *user_ns);
int vns_alloc_pidnr(struct vns_pid_namespace *ns);
int vns_get_pid_max(void);
void vns_free_pidnr(struct vns_pid_namespace *ns, int nr);
void vns_zap_pid_ns(struct vns_pid_namespace *ns);
void vns_free_pid_ns(struct vns_pid_namespace *ns);

/* ------------------------------------------------------------------ */
/* vns_generic.c - ipc/mnt/net/cgroup/time bookkeeping		*/
/* ------------------------------------------------------------------ */

struct vns_generic_namespace *vns_generic_root(u32 type);
struct vns_generic_namespace *vns_create_generic_ns(u32 type);
void vns_free_generic_ns(struct vns_generic_namespace *ns);

/* ------------------------------------------------------------------ */
/* vns_nsproxy.c - nsproxy + per-task-group membership		*/
/* ------------------------------------------------------------------ */

struct vns_nsproxy *vns_nsproxy_root(void);
struct vns_nsproxy *vns_create_nsproxy(unsigned long flags,
				       struct vns_nsproxy *old);
void vns_get_nsproxy(struct vns_nsproxy *nsp);
void vns_put_nsproxy(struct vns_nsproxy *nsp);

struct vns_task *vns_task_lookup(pid_t tgid);
struct vns_nsproxy *vns_current_nsproxy(bool create);
int vns_task_set_nsproxy(pid_t tgid, struct vns_nsproxy *nsp);
void vns_task_forget(pid_t tgid);
void vns_task_purge_all(void);

int vns_do_unshare(unsigned long flags);
int vns_do_setns_flags(unsigned long flags);
void vns_track_child(pid_t child_tgid);
void vns_track_child_flags(pid_t child_tgid, unsigned long flags);

/*
 * Current-namespace operations used by the syscall hooks. All acquire
 * vendor_ns_registry.lock internally and operate on the calling task group's
 * vendored namespaces.
 */
int vns_uts_set(const char *name, size_t len, bool domain);
int vns_uts_get(char *out, size_t outlen, bool domain);
pid_t vns_pid_translate(pid_t real);
uid_t vns_uid_translate(uid_t id);
gid_t vns_gid_translate(gid_t id);
bool vns_pid_visible(pid_t real);
bool vns_in_child_pidns(void);

/* ------------------------------------------------------------------ */
/* vendor_ns_syscalls.c - syscall arg helpers + hook tables	    */
/* ------------------------------------------------------------------ */

unsigned long vns_sys_arg0(const struct pt_regs *regs);
unsigned long vns_sys_arg1(const struct pt_regs *regs);
unsigned long vns_sys_arg2(const struct pt_regs *regs);
unsigned long vns_sys_arg3(const struct pt_regs *regs);

struct shadow_hook;
extern struct shadow_hook *vendor_ns_core_hooks[];
extern struct shadow_hook *vendor_ns_uts_hooks[];
extern struct shadow_hook *vendor_ns_pid_hooks[];
extern struct shadow_hook *vendor_ns_user_hooks[];

/* ------------------------------------------------------------------ */
/* vendor_ns_procfs.c - /proc getdents64 pid filtering		    */
/* ------------------------------------------------------------------ */

extern struct shadow_hook *vendor_ns_procfs_hooks[];

/* ------------------------------------------------------------------ */
/* vendor_ns_diag.c - diagfs rendering				    */
/* ------------------------------------------------------------------ */

size_t vendor_ns_diag_snprintf(char *buf, size_t buflen);
size_t vendor_ns_diag_snprintf_type(u32 type, char *buf, size_t buflen);

/* ------------------------------------------------------------------ */
/* vendor_ns_module.c - lifecycle					*/
/* ------------------------------------------------------------------ */

int vendor_ns_init(void);
void vendor_ns_exit(void);

#endif /* _VENDOR_NS_INTERNAL_H */
