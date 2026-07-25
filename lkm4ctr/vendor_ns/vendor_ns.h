/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vendor_ns.h - internal header for the vendor_ns submodule.
 * vendor_ns is NEW code (not vendored from kernel).
 */
#ifndef _VENDOR_NS_H
#define _VENDOR_NS_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/version.h>
#include <linux/kref.h>
#include <linux/hashtable.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/refcount.h>
#include <linux/nsproxy.h>
#include <linux/proc_ns.h>
#include <linux/utsname.h>
#include <linux/pid_namespace.h>
#include <linux/user_namespace.h>
#include <linux/ipc_namespace.h>
#include <linux/time_namespace.h>
#include <linux/cgroup.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/fs_struct.h>
#include <linux/file.h>
#include <net/net_namespace.h>

struct vns_task;
struct shadow_hook;

#define VNS_TASK_HASH_BITS 10
#define VNS_CLONE_FLAGS ((unsigned long)(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | \
				CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUSER | \
				CLONE_NEWCGROUP | CLONE_NEWTIME))

struct vns_task {
	pid_t tgid;
	struct nsproxy *nsproxy;
	struct hlist_node node;
};

struct vns_registry {
	DECLARE_HASHTABLE(tasks, VNS_TASK_HASH_BITS);
	spinlock_t lock;
	unsigned long task_count;
	unsigned long stat_unshare;
	unsigned long stat_setns;
	unsigned long stat_clone;
};

extern struct vns_registry vendor_ns_registry;
extern int (*vns_proc_alloc_inum_fn)(unsigned int *);
extern void (*vns_proc_free_inum_fn)(unsigned int);
extern struct mnt_namespace *(*vns_copy_mnt_ns_fn)(unsigned long, struct mnt_namespace *, struct user_namespace *, struct fs_struct *);
extern void (*vns_put_mnt_ns_fn)(struct mnt_namespace *);
extern struct net *(*vns_copy_net_ns_fn)(unsigned long, struct user_namespace *, struct net *);
extern void (*vns_put_net_ns_fn)(struct net *);
extern bool vendor_ns_enabled;

static inline void vns_count_set(void *count, int value, bool is_refcount)
{
	if (is_refcount)
		refcount_set((refcount_t *)count, value);
	else
		atomic_set((atomic_t *)count, value);
}

static inline bool vns_count_dec_and_test(void *count, bool is_refcount)
{
	if (is_refcount)
		return refcount_dec_and_test((refcount_t *)count);
	return atomic_dec_and_test((atomic_t *)count);
}

#define VNS_COUNT_TYPE_IS_REFCOUNT(ptr) \
	__builtin_types_compatible_p(typeof(*(ptr)), refcount_t)

#define vns_init_count(ptr, value) \
	vns_count_set((void *)(ptr), (value), VNS_COUNT_TYPE_IS_REFCOUNT(ptr))

#define vns_put_count(ptr) \
	vns_count_dec_and_test((void *)(ptr), VNS_COUNT_TYPE_IS_REFCOUNT(ptr))

static inline void vns_zero_stashed(struct ns_common *ns)
{
	memset(&ns->stashed, 0, sizeof(ns->stashed));
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#define vns_uts_init_ref(obj) vns_init_count(&(obj)->kref.refcount, 1)
#define vns_pid_init_ref(obj) vns_init_count(&(obj)->kref.refcount, 1)
#define vns_pid_put_ref(obj) vns_put_count(&(obj)->kref.refcount)
#define vns_user_init_ref(obj) vns_init_count(&(obj)->count, 1)
#define vns_user_put_ref(obj) vns_put_count(&(obj)->count)
#define vns_ipc_init_ref(obj) vns_init_count(&(obj)->count, 1)
#define vns_ipc_put_ref_lock(obj, lock) refcount_dec_and_lock(&(obj)->count, (lock))
#define VNS_TIME_REF_INIT .kref = KREF_INIT(1),
#else
#define vns_uts_init_ref(obj) vns_init_count(&(obj)->ns.count, 1)
#define vns_pid_init_ref(obj) vns_init_count(&(obj)->ns.count, 1)
#define vns_pid_put_ref(obj) vns_put_count(&(obj)->ns.count)
#define vns_user_init_ref(obj) vns_init_count(&(obj)->ns.count, 1)
#define vns_user_put_ref(obj) vns_put_count(&(obj)->ns.count)
#define vns_ipc_init_ref(obj) vns_init_count(&(obj)->ns.count, 1)
#define vns_ipc_put_ref_lock(obj, lock) refcount_dec_and_lock(&(obj)->ns.count, (lock))
#define VNS_TIME_REF_INIT .ns.count = REFCOUNT_INIT(1),
#endif

int vns_alloc_inum(struct ns_common *ns);
void vns_free_inum(struct ns_common *ns);

struct uts_namespace *vns_copy_utsname(unsigned long flags, struct user_namespace *user_ns, struct uts_namespace *old_ns);
void vns_free_uts_ns(struct uts_namespace *ns);
extern const struct proc_ns_operations vns_utsns_operations;
void vns_uts_ns_init(void);

extern struct nsproxy vns_init_nsproxy;
struct nsproxy *vns_copy_namespaces(unsigned long flags, struct task_struct *tsk);
void vns_free_nsproxy(struct nsproxy *ns);
void vns_put_nsproxy(struct nsproxy *ns);
int vns_unshare_nsproxy_namespaces(unsigned long unshare_flags, struct nsproxy **new_nsproxy, struct cred *new_cred, struct fs_struct *new_fs);
void vns_switch_task_namespaces(struct task_struct *p, struct nsproxy *new);
void vns_exit_task_namespaces(struct task_struct *p);
long vns_sys_setns(int fd, int flags);

struct ipc_namespace *vns_copy_ipcs(unsigned long flags, struct user_namespace *user_ns, struct ipc_namespace *old_ns);
void vns_put_ipc_ns(struct ipc_namespace *ns);
extern const struct proc_ns_operations vns_ipcns_operations;

struct cgroup_namespace *vns_copy_cgroup_ns(unsigned long flags, struct user_namespace *user_ns, struct cgroup_namespace *old_cgroup_ns);
void vns_put_cgroup_ns(struct cgroup_namespace *ns);
extern const struct proc_ns_operations vns_cgroupns_operations;

extern struct time_namespace vns_init_time_ns;
struct time_namespace *vns_copy_time_ns(unsigned long flags, struct user_namespace *user_ns, struct time_namespace *old_ns);
void vns_put_time_ns(struct time_namespace *ns);
void vns_free_time_ns(struct time_namespace *ns);
void vns_timens_commit(struct task_struct *tsk, struct time_namespace *ns);
void vns_timens_on_fork(struct nsproxy *nsproxy, struct task_struct *tsk);
void vns_proc_timens_show_offsets(struct task_struct *p, struct seq_file *m);
int vns_proc_timens_set_offset(struct file *file, struct task_struct *p, struct proc_timens_offset *offsets, int noffsets);
extern const struct proc_ns_operations vns_timens_operations;
extern const struct proc_ns_operations vns_timens_for_children_operations;

struct pid_namespace *vns_copy_pid_ns(unsigned long flags, struct user_namespace *user_ns, struct pid_namespace *old_ns);
void vns_put_pid_ns(struct pid_namespace *ns);
void vns_zap_pid_ns_processes(struct pid_namespace *pid_ns);
int vns_reboot_pid_ns(struct pid_namespace *pid_ns, int cmd);
extern const struct proc_ns_operations vns_pidns_operations;
extern const struct proc_ns_operations vns_pidns_for_children_operations;
void vns_pid_ns_init(void);

int vns_create_user_ns(struct cred *new);
int vns_unshare_userns(unsigned long unshare_flags, struct cred **new_cred);
void vns___put_user_ns(struct user_namespace *ns);
kuid_t vns_make_kuid(struct user_namespace *ns, uid_t uid);
uid_t vns_from_kuid(struct user_namespace *targ, kuid_t kuid);
uid_t vns_from_kuid_munged(struct user_namespace *targ, kuid_t kuid);
kgid_t vns_make_kgid(struct user_namespace *ns, gid_t gid);
gid_t vns_from_kgid(struct user_namespace *targ, kgid_t kgid);
gid_t vns_from_kgid_munged(struct user_namespace *targ, kgid_t kgid);
kprojid_t vns_make_kprojid(struct user_namespace *ns, projid_t projid);
projid_t vns_from_kprojid(struct user_namespace *targ, kprojid_t kprojid);
projid_t vns_from_kprojid_munged(struct user_namespace *targ, kprojid_t kprojid);
ssize_t vns_proc_uid_map_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos);
ssize_t vns_proc_gid_map_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos);
ssize_t vns_proc_projid_map_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos);
int vns_proc_setgroups_show(struct seq_file *seq, void *v);
ssize_t vns_proc_setgroups_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos);
bool vns_userns_may_setgroups(const struct user_namespace *ns);
bool vns_in_userns(const struct user_namespace *ancestor, const struct user_namespace *child);
bool vns_current_in_userns(const struct user_namespace *target_ns);
struct ns_common *vns_ns_get_owner(struct ns_common *ns);
extern const struct proc_ns_operations vns_userns_operations;
void vns_user_ns_init(void);

void vns_nsfs_init(void);

struct vns_task *vns_task_find(pid_t tgid);
struct nsproxy *vns_current_nsproxy(void);
int vns_registry_set_nsproxy(pid_t tgid, struct nsproxy *nsproxy);
void vns_registry_remove(pid_t tgid);
void vns_registry_clone(pid_t parent_tgid, pid_t child_tgid);

extern struct shadow_hook *vendor_ns_core_hooks[];

/* compat layer (glue/vendor_ns_compat.c) */
extern struct ucounts vns_ucounts_stub;
#ifdef CONFIG_CGROUPS
extern struct cgroup_namespace *vns_init_cgroup_ns_ptr;
#endif
#if defined(CONFIG_POSIX_MQUEUE) || defined(CONFIG_SYSVIPC)
extern struct ipc_namespace *vns_init_ipc_ns_ptr;
#endif
void vns_compat_resolve(void);
bool vns_compat_ready(void);

int vns_security_create_user_ns(const struct cred *cred);
bool vns_setup_mq_sysctls(struct ipc_namespace *ns);
void vns_retire_mq_sysctls(struct ipc_namespace *ns);
bool vns_setup_ipc_sysctls(struct ipc_namespace *ns);
void vns_retire_ipc_sysctls(struct ipc_namespace *ns);
int set_cred_ucounts(struct cred *new);

#ifndef VNS_COMPAT_IMPL
#define security_create_user_ns vns_security_create_user_ns
#define setup_mq_sysctls vns_setup_mq_sysctls
#define retire_mq_sysctls vns_retire_mq_sysctls
#define setup_ipc_sysctls vns_setup_ipc_sysctls
#define retire_ipc_sysctls vns_retire_ipc_sysctls
#endif

int vendor_ns_init(void);
void vendor_ns_exit(void);
size_t vendor_ns_diag_snprintf(char *buf, size_t buflen);

#endif /* _VENDOR_NS_H */
