// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_ipc_compat.c - local resolvers/stubs for the kernel-internal
 * symbols pulled in by the vendored SysV IPC / POSIX mqueue sources
 * (ipc/msg.c, ipc/sem.c, ipc/shm.c, ipc/mqueue.c, ipc/util.c).
 *
 * This is NEW code (not vendored from kernel-common); same idea and same
 * safety contract as glue/vendor_kernel_compat.c (read its header comment
 * first). The vendored ipc files reference many symbols that the kernel does
 * NOT export via EXPORT_SYMBOL*, so an out-of-tree module cannot link against
 * them directly. For each one we provide a module-local definition under the
 * *real* kernel name (so the existing call sites in the vendored .c files need
 * no edits) that forwards to the real kernel function resolved by name via
 * shadow_hook_resolve() at init time, falling back to a safe stub when the
 * symbol could not be resolved.
 *
 * Unlike glue/vendor_kernel_compat.c (which mostly uses vns_-prefixed names +
 * #define aliases in vendor_kernel.h), these symbols are referenced from
 * <linux/audit.h>/<linux/hugetlb.h>/etc. inline helpers that are expanded
 * before any vendor_kernel.h #define could rename them, so we must define the
 * real names directly here (mirroring how glue/vendor_kernel_compat.c defines
 * tasklist_lock / free_uts_ns / free_cgroup_ns directly).
 *
 * Resolution of every function symbol below is reliable on real targets:
 * these are all ordinary kallsyms *function* symbols (not the private
 * `struct kmem_cache *` *data* symbols that forced the namespace caches to be
 * module-owned - see vendor_kernel/README.md, "Slab-cache consistency with the
 * real kernel"). The stubs only ever run on a hypothetical kernel where a
 * symbol is genuinely absent, in which case the corresponding IPC feature
 * degrades gracefully (LSM checks allow, audit records are skipped, blocking
 * wakeups/signals are dropped) rather than crashing.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/security.h>
#include <linux/audit.h>
#include <linux/sched/wake_q.h>
#include <linux/signal.h>
#include <linux/user_namespace.h>
#include <linux/netlink.h>
#include <linux/hugetlb.h>
#include <linux/shmem_fs.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/sem.h>
#include <linux/mqueue.h>
#include <linux/namei.h>

#include "../vendor_kernel.h"
#include "../ipc/util.h"
#include "../../../common/shadow_hook.h"
#include "../../../common/lkm4ctr_log.h"

/* ---- ipc/util.h data symbols (normally ipc/ipc_sysctl.c, not vendored) --- */
/*
 * Under CONFIG_SYSVIPC_SYSCTL, ipc/util.h declares these extern (they live in
 * ipc/ipc_sysctl.c, which is not vendored). vendor_kernel does not vendor the
 * ipc sysctl machinery (setup_ipc_sysctls() is stubbed), so define them here
 * with the same defaults the kernel uses when the "ipcmni_extend" boot option
 * is absent. They only bound the ipc id space; the defaults are correct for
 * the non-extended layout.
 */
#ifdef CONFIG_SYSVIPC_SYSCTL
int ipc_mni = IPCMNI;
int ipc_mni_shift = IPCMNI_SHIFT;
int ipc_min_cycle = RADIX_TREE_MAP_SIZE;
#endif

/*
 * sysctl_overcommit_memory (mm/util.c, not exported) - only read by shm.c to
 * decide SHM_NORESERVE accounting. Default OVERCOMMIT_GUESS matches the kernel.
 */
int sysctl_overcommit_memory = OVERCOMMIT_GUESS;

/*
 * hugetlb data symbols (mm/hugetlb.c, not exported). Only reached via the
 * SHM_HUGETLB path in shm.c, which is a documented best-effort gap for
 * vendor_kernel (see README). Defining zeroed module-local storage keeps the
 * <linux/hugetlb.h> inline helpers (default_hstate, hstate_sizelog) linkable;
 * size_to_hstate() returns NULL below, so shmget(SHM_HUGETLB) fails cleanly
 * with -EINVAL rather than touching this zeroed state.
 */
struct hstate hstates[HUGE_MAX_HSTATE];
unsigned int default_hstate_idx;

/* ---- resolved function pointers --------------------------------------- */

/* mm */
typedef int (*do_munmap_fn_t)(struct mm_struct *, unsigned long, size_t,
	struct list_head *);
typedef int (*mm_populate_fn_t)(unsigned long, unsigned long, int);
typedef int (*security_mmap_file_fn_t)(struct file *, unsigned long, unsigned long);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
typedef unsigned long (*do_mmap_fn_t)(struct file *, unsigned long,
	unsigned long, unsigned long, unsigned long, unsigned long,
	unsigned long *, struct list_head *);
#else
typedef unsigned long (*do_mmap_fn_t)(struct file *, unsigned long,
	unsigned long, unsigned long, unsigned long, vm_flags_t,
	unsigned long, unsigned long *, struct list_head *);
#endif
#if defined(CONFIG_HUGETLBFS)
typedef struct file *(*hugetlb_file_setup_fn_t)(const char *, size_t, vm_flags_t,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	struct ucounts **,
#endif
	int, int);
#endif
#if defined(CONFIG_HUGETLB_PAGE)
typedef struct hstate *(*size_to_hstate_fn_t)(unsigned long);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
typedef struct user_struct vns_shmem_lock_owner_t;
#else
typedef struct ucounts vns_shmem_lock_owner_t;
#endif
typedef int (*shmem_lock_fn_t)(struct file *, int, vns_shmem_lock_owner_t *);
typedef void (*shmem_unlock_mapping_fn_t)(struct address_space *);
typedef struct file *(*alloc_file_clone_fn_t)(struct file *, int,
	const struct file_operations *);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
typedef struct filename *(*getname_flags_fn_t)(const char __user *, int, int *);
#else
typedef struct filename *(*getname_flags_fn_t)(const char __user *, int);
#endif

/* signal / wake_q */
typedef int (*do_send_sig_info_fn_t)(int, struct kernel_siginfo *,
	struct task_struct *, enum pid_type);
typedef void (*wake_q_add_fn_t)(struct wake_q_head *, struct task_struct *);
typedef void (*wake_q_add_safe_fn_t)(struct wake_q_head *, struct task_struct *);
typedef void (*wake_up_q_fn_t)(struct wake_q_head *);

/* ucounts */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
typedef void (*put_ucounts_fn_t)(struct ucounts *);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
typedef enum ucount_type vns_rlimit_ucount_type_t;
#else
typedef enum rlimit_type vns_rlimit_ucount_type_t;
#endif
typedef long (*inc_rlimit_ucounts_fn_t)(struct ucounts *,
					vns_rlimit_ucount_type_t, long);
typedef bool (*dec_rlimit_ucounts_fn_t)(struct ucounts *,
					vns_rlimit_ucount_type_t, long);
#endif

/* netlink (mq_notify) */
typedef struct sock *(*netlink_getsockbyfd_fn_t)(int);
typedef int (*netlink_attachskb_fn_t)(struct sock *, struct sk_buff *, long *,
	struct sock *);
typedef void (*netlink_detachskb_fn_t)(struct sock *, struct sk_buff *);
typedef int (*netlink_sendskb_fn_t)(struct sock *, struct sk_buff *);

/* audit */
typedef void (*audit_inode_fn_t)(struct filename *, const struct dentry *,
	unsigned int);
typedef void (*audit_file_fn_t)(const struct file *);
typedef void (*audit_ipc_obj_fn_t)(struct kern_ipc_perm *);
typedef void (*audit_ipc_set_perm_fn_t)(unsigned long, uid_t, gid_t, umode_t);
typedef void (*audit_mq_open_fn_t)(int, umode_t, struct mq_attr *);
typedef void (*audit_mq_sendrecv_fn_t)(mqd_t, size_t, unsigned int,
	const struct timespec64 *);
typedef void (*audit_mq_notify_fn_t)(mqd_t, const struct sigevent *);
typedef void (*audit_mq_getsetattr_fn_t)(mqd_t, struct mq_attr *);

/* security ipc/msg/sem/shm */
typedef int (*sec_ipc_permission_fn_t)(struct kern_ipc_perm *, short);
typedef int (*sec_msg_msg_alloc_fn_t)(struct msg_msg *);
typedef void (*sec_msg_msg_free_fn_t)(struct msg_msg *);
typedef int (*sec_msg_queue_alloc_fn_t)(struct kern_ipc_perm *);
typedef void (*sec_msg_queue_free_fn_t)(struct kern_ipc_perm *);
typedef int (*sec_msg_queue_associate_fn_t)(struct kern_ipc_perm *, int);
typedef int (*sec_msg_queue_msgctl_fn_t)(struct kern_ipc_perm *, int);
typedef int (*sec_msg_queue_msgsnd_fn_t)(struct kern_ipc_perm *, struct msg_msg *, int);
typedef int (*sec_msg_queue_msgrcv_fn_t)(struct kern_ipc_perm *, struct msg_msg *,
	struct task_struct *, long, int);
typedef int (*sec_sem_alloc_fn_t)(struct kern_ipc_perm *);
typedef void (*sec_sem_free_fn_t)(struct kern_ipc_perm *);
typedef int (*sec_sem_associate_fn_t)(struct kern_ipc_perm *, int);
typedef int (*sec_sem_semctl_fn_t)(struct kern_ipc_perm *, int);
typedef int (*sec_sem_semop_fn_t)(struct kern_ipc_perm *, struct sembuf *,
	unsigned, int);
typedef int (*sec_shm_alloc_fn_t)(struct kern_ipc_perm *);
typedef void (*sec_shm_free_fn_t)(struct kern_ipc_perm *);
typedef int (*sec_shm_associate_fn_t)(struct kern_ipc_perm *, int);
typedef int (*sec_shm_shmctl_fn_t)(struct kern_ipc_perm *, int);
typedef int (*sec_shm_shmat_fn_t)(struct kern_ipc_perm *, char __user *, int);

static do_munmap_fn_t           r_do_munmap;
static mm_populate_fn_t         r_mm_populate;
static security_mmap_file_fn_t  r_security_mmap_file;
static do_mmap_fn_t             r_do_mmap;
#if defined(CONFIG_HUGETLBFS)
static hugetlb_file_setup_fn_t  r_hugetlb_file_setup;
#endif
#if defined(CONFIG_HUGETLB_PAGE)
static size_to_hstate_fn_t      r_size_to_hstate;
#endif
static shmem_lock_fn_t          r_shmem_lock;
static shmem_unlock_mapping_fn_t r_shmem_unlock_mapping;
static alloc_file_clone_fn_t    r_alloc_file_clone;
static getname_flags_fn_t       r_getname_flags;
static do_send_sig_info_fn_t    r_do_send_sig_info;
static wake_q_add_fn_t          r_wake_q_add;
static wake_q_add_safe_fn_t     r_wake_q_add_safe;
static wake_up_q_fn_t           r_wake_up_q;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
static put_ucounts_fn_t         r_put_ucounts;
static inc_rlimit_ucounts_fn_t  r_inc_rlimit_ucounts;
static dec_rlimit_ucounts_fn_t  r_dec_rlimit_ucounts;
#endif
static netlink_getsockbyfd_fn_t r_netlink_getsockbyfd;
static netlink_attachskb_fn_t   r_netlink_attachskb;
static netlink_detachskb_fn_t   r_netlink_detachskb;
static netlink_sendskb_fn_t     r_netlink_sendskb;
static audit_inode_fn_t         r_audit_inode;
static audit_file_fn_t          r_audit_file;
static audit_ipc_obj_fn_t       r_audit_ipc_obj;
static audit_ipc_set_perm_fn_t  r_audit_ipc_set_perm;
static audit_mq_open_fn_t        r_audit_mq_open;
static audit_mq_sendrecv_fn_t    r_audit_mq_sendrecv;
static audit_mq_notify_fn_t      r_audit_mq_notify;
static audit_mq_getsetattr_fn_t  r_audit_mq_getsetattr;
static sec_ipc_permission_fn_t   r_sec_ipc_permission;
static sec_msg_msg_alloc_fn_t    r_sec_msg_msg_alloc;
static sec_msg_msg_free_fn_t     r_sec_msg_msg_free;
static sec_msg_queue_alloc_fn_t  r_sec_msg_queue_alloc;
static sec_msg_queue_free_fn_t   r_sec_msg_queue_free;
static sec_msg_queue_associate_fn_t r_sec_msg_queue_associate;
static sec_msg_queue_msgctl_fn_t r_sec_msg_queue_msgctl;
static sec_msg_queue_msgsnd_fn_t r_sec_msg_queue_msgsnd;
static sec_msg_queue_msgrcv_fn_t r_sec_msg_queue_msgrcv;
static sec_sem_alloc_fn_t        r_sec_sem_alloc;
static sec_sem_free_fn_t         r_sec_sem_free;
static sec_sem_associate_fn_t    r_sec_sem_associate;
static sec_sem_semctl_fn_t       r_sec_sem_semctl;
static sec_sem_semop_fn_t        r_sec_sem_semop;
static sec_shm_alloc_fn_t        r_sec_shm_alloc;
static sec_shm_free_fn_t         r_sec_shm_free;
static sec_shm_associate_fn_t    r_sec_shm_associate;
static sec_shm_shmctl_fn_t       r_sec_shm_shmctl;
static sec_shm_shmat_fn_t        r_sec_shm_shmat;

void vns_ipc_compat_resolve(void)
{
#define R(var, sym) \
	do { \
		(var) = (typeof(var))(uintptr_t)shadow_hook_resolve(#sym); \
		if (!(var)) \
			LKM4CTR_WARN("vendor_kernel", \
				"ipc compat: " #sym " not resolved (stub active)"); \
	} while (0)

	R(r_do_munmap, do_munmap);
	R(r_mm_populate, __mm_populate);
	R(r_security_mmap_file, security_mmap_file);
	R(r_do_mmap, do_mmap);
#if defined(CONFIG_HUGETLBFS)
	R(r_hugetlb_file_setup, hugetlb_file_setup);
#endif
#if defined(CONFIG_HUGETLB_PAGE)
	R(r_size_to_hstate, size_to_hstate);
#endif
	R(r_shmem_lock, shmem_lock);
	R(r_shmem_unlock_mapping, shmem_unlock_mapping);
	R(r_alloc_file_clone, alloc_file_clone);
	R(r_getname_flags, getname_flags);
	R(r_do_send_sig_info, do_send_sig_info);
	R(r_wake_q_add, wake_q_add);
	R(r_wake_q_add_safe, wake_q_add_safe);
	R(r_wake_up_q, wake_up_q);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	R(r_put_ucounts, put_ucounts);
	R(r_inc_rlimit_ucounts, inc_rlimit_ucounts);
	R(r_dec_rlimit_ucounts, dec_rlimit_ucounts);
#endif
	R(r_netlink_getsockbyfd, netlink_getsockbyfd);
	R(r_netlink_attachskb, netlink_attachskb);
	R(r_netlink_detachskb, netlink_detachskb);
	R(r_netlink_sendskb, netlink_sendskb);
	R(r_audit_inode, __audit_inode);
	R(r_audit_file, __audit_file);
	R(r_audit_ipc_obj, __audit_ipc_obj);
	R(r_audit_ipc_set_perm, __audit_ipc_set_perm);
	R(r_audit_mq_open, __audit_mq_open);
	R(r_audit_mq_sendrecv, __audit_mq_sendrecv);
	R(r_audit_mq_notify, __audit_mq_notify);
	R(r_audit_mq_getsetattr, __audit_mq_getsetattr);
	R(r_sec_ipc_permission, security_ipc_permission);
	R(r_sec_msg_msg_alloc, security_msg_msg_alloc);
	R(r_sec_msg_msg_free, security_msg_msg_free);
	R(r_sec_msg_queue_alloc, security_msg_queue_alloc);
	R(r_sec_msg_queue_free, security_msg_queue_free);
	R(r_sec_msg_queue_associate, security_msg_queue_associate);
	R(r_sec_msg_queue_msgctl, security_msg_queue_msgctl);
	R(r_sec_msg_queue_msgsnd, security_msg_queue_msgsnd);
	R(r_sec_msg_queue_msgrcv, security_msg_queue_msgrcv);
	R(r_sec_sem_alloc, security_sem_alloc);
	R(r_sec_sem_free, security_sem_free);
	R(r_sec_sem_associate, security_sem_associate);
	R(r_sec_sem_semctl, security_sem_semctl);
	R(r_sec_sem_semop, security_sem_semop);
	R(r_sec_shm_alloc, security_shm_alloc);
	R(r_sec_shm_free, security_shm_free);
	R(r_sec_shm_associate, security_shm_associate);
	R(r_sec_shm_shmctl, security_shm_shmctl);
	R(r_sec_shm_shmat, security_shm_shmat);
#undef R
}

/* ---- mm ---------------------------------------------------------------- */

int do_munmap(struct mm_struct *mm, unsigned long start, size_t len,
	      struct list_head *uf)
{
	if (r_do_munmap)
		return r_do_munmap(mm, start, len, uf);
	return -ENOSYS;
}

int __mm_populate(unsigned long start, unsigned long len, int ignore_errors)
{
	if (r_mm_populate)
		return r_mm_populate(start, len, ignore_errors);
	return 0;
}

int security_mmap_file(struct file *file, unsigned long prot, unsigned long flags)
{
	if (r_security_mmap_file)
		return r_security_mmap_file(file, prot, flags);
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
unsigned long do_mmap(struct file *file, unsigned long addr, unsigned long len,
		      unsigned long prot, unsigned long flags,
		      unsigned long pgoff, unsigned long *populate,
		      struct list_head *uf)
{
	if (r_do_mmap)
		return r_do_mmap(file, addr, len, prot, flags, pgoff, populate, uf);
	return -ENOSYS;
}
#else
unsigned long do_mmap(struct file *file, unsigned long addr, unsigned long len,
		      unsigned long prot, unsigned long flags,
		      vm_flags_t vm_flags, unsigned long pgoff,
		      unsigned long *populate, struct list_head *uf)
{
	if (r_do_mmap)
		return r_do_mmap(file, addr, len, prot, flags, vm_flags, pgoff,
				 populate, uf);
	return -ENOSYS;
}
#endif

#if defined(CONFIG_HUGETLBFS)
struct file *hugetlb_file_setup(const char *name, size_t size, vm_flags_t acct,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
				struct ucounts **ucounts,
#endif
				int creat_flags, int page_size_log)
{
	if (r_hugetlb_file_setup)
		return r_hugetlb_file_setup(name, size, acct,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
					    ucounts,
#endif
					    creat_flags,
					    page_size_log);
	return ERR_PTR(-ENOSYS);
}
#endif

#if defined(CONFIG_HUGETLB_PAGE)
struct hstate *size_to_hstate(unsigned long size)
{
	if (r_size_to_hstate)
		return r_size_to_hstate(size);
	return NULL;
}
#endif

int shmem_lock(struct file *file, int lock, vns_shmem_lock_owner_t *owner)
{
	if (r_shmem_lock)
		return r_shmem_lock(file, lock, owner);
	return 0;
}

void shmem_unlock_mapping(struct address_space *mapping)
{
	if (r_shmem_unlock_mapping)
		r_shmem_unlock_mapping(mapping);
}

struct file *alloc_file_clone(struct file *base, int flags,
			      const struct file_operations *fops)
{
	if (r_alloc_file_clone)
		return r_alloc_file_clone(base, flags, fops);
	return ERR_PTR(-ENOSYS);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
struct filename *getname_flags(const char __user *filename, int flags, int *empty)
{
	if (r_getname_flags)
		return r_getname_flags(filename, flags, empty);
	return ERR_PTR(-ENOSYS);
}
#else
struct filename *getname_flags(const char __user *filename, int flags)
{
	if (r_getname_flags)
		return r_getname_flags(filename, flags);
	return ERR_PTR(-ENOSYS);
}
#endif

/* ---- signal / wake_q --------------------------------------------------- */

int do_send_sig_info(int sig, struct kernel_siginfo *info,
		     struct task_struct *p, enum pid_type type)
{
	if (r_do_send_sig_info)
		return r_do_send_sig_info(sig, info, p, type);
	return -ENOSYS;
}

void wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	if (r_wake_q_add)
		r_wake_q_add(head, task);
}

void wake_q_add_safe(struct wake_q_head *head, struct task_struct *task)
{
	if (r_wake_q_add_safe) {
		r_wake_q_add_safe(head, task);
		return;
	}
	/*
	 * The real wake_q_add_safe() consumes the reference put_task_struct()
	 * that the caller took; drop it here so the stub does not leak.
	 */
	put_task_struct(task);
}

void wake_up_q(struct wake_q_head *head)
{
	if (r_wake_up_q)
		r_wake_up_q(head);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
/* ---- ucounts ----------------------------------------------------------- */

void put_ucounts(struct ucounts *ucounts)
{
	if (r_put_ucounts)
		r_put_ucounts(ucounts);
}

long inc_rlimit_ucounts(struct ucounts *ucounts, vns_rlimit_ucount_type_t type,
			long v)
{
	if (r_inc_rlimit_ucounts)
		return r_inc_rlimit_ucounts(ucounts, type, v);
	return LONG_MAX; /* stub: pretend headroom, no enforcement */
}

bool dec_rlimit_ucounts(struct ucounts *ucounts, vns_rlimit_ucount_type_t type,
			long v)
{
	if (r_dec_rlimit_ucounts)
		return r_dec_rlimit_ucounts(ucounts, type, v);
	return false;
}
#endif

/* ---- netlink (mq_notify) ----------------------------------------------- */

struct sock *netlink_getsockbyfd(int fd)
{
	if (r_netlink_getsockbyfd)
		return r_netlink_getsockbyfd(fd);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
	{
		struct fd f = fdget(fd);
		struct sock *sock;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
		if (fd_empty(f))
			return ERR_PTR(-EBADF);
		sock = netlink_getsockbyfilp(fd_file(f));
#else
		if (!f.file)
			return ERR_PTR(-EBADF);
		sock = netlink_getsockbyfilp(f.file);
#endif
		fdput(f);
		return sock;
	}
#endif
	return ERR_PTR(-ENOSYS);
}

int netlink_attachskb(struct sock *sk, struct sk_buff *skb, long *timeo,
		      struct sock *ssk)
{
	if (r_netlink_attachskb)
		return r_netlink_attachskb(sk, skb, timeo, ssk);
	return -ENOSYS;
}

void netlink_detachskb(struct sock *sk, struct sk_buff *skb)
{
	if (r_netlink_detachskb)
		r_netlink_detachskb(sk, skb);
}

int netlink_sendskb(struct sock *sk, struct sk_buff *skb)
{
	if (r_netlink_sendskb)
		return r_netlink_sendskb(sk, skb);
	return -ENOSYS;
}

/* ---- audit ------------------------------------------------------------- */

void __audit_inode(struct filename *name, const struct dentry *dentry,
		   unsigned int flags)
{
	if (r_audit_inode)
		r_audit_inode(name, dentry, flags);
}

void __audit_file(const struct file *file)
{
	if (r_audit_file)
		r_audit_file(file);
}

void __audit_ipc_obj(struct kern_ipc_perm *ipcp)
{
	if (r_audit_ipc_obj)
		r_audit_ipc_obj(ipcp);
}

void __audit_ipc_set_perm(unsigned long qbytes, uid_t uid, gid_t gid,
			  umode_t mode)
{
	if (r_audit_ipc_set_perm)
		r_audit_ipc_set_perm(qbytes, uid, gid, mode);
}

void __audit_mq_open(int oflag, umode_t mode, struct mq_attr *attr)
{
	if (r_audit_mq_open)
		r_audit_mq_open(oflag, mode, attr);
}

void __audit_mq_sendrecv(mqd_t mqdes, size_t msg_len, unsigned int msg_prio,
			 const struct timespec64 *abs_timeout)
{
	if (r_audit_mq_sendrecv)
		r_audit_mq_sendrecv(mqdes, msg_len, msg_prio, abs_timeout);
}

void __audit_mq_notify(mqd_t mqdes, const struct sigevent *notification)
{
	if (r_audit_mq_notify)
		r_audit_mq_notify(mqdes, notification);
}

void __audit_mq_getsetattr(mqd_t mqdes, struct mq_attr *mqstat)
{
	if (r_audit_mq_getsetattr)
		r_audit_mq_getsetattr(mqdes, mqstat);
}

/* ---- security (ipc/msg/sem/shm) --------------------------------------- */

int security_ipc_permission(struct kern_ipc_perm *ipcp, short flag)
{
	if (r_sec_ipc_permission)
		return r_sec_ipc_permission(ipcp, flag);
	return 0;
}

int security_msg_msg_alloc(struct msg_msg *msg)
{
	if (r_sec_msg_msg_alloc)
		return r_sec_msg_msg_alloc(msg);
	return 0;
}

void security_msg_msg_free(struct msg_msg *msg)
{
	if (r_sec_msg_msg_free)
		r_sec_msg_msg_free(msg);
}

int security_msg_queue_alloc(struct kern_ipc_perm *msq)
{
	if (r_sec_msg_queue_alloc)
		return r_sec_msg_queue_alloc(msq);
	return 0;
}

void security_msg_queue_free(struct kern_ipc_perm *msq)
{
	if (r_sec_msg_queue_free)
		r_sec_msg_queue_free(msq);
}

int security_msg_queue_associate(struct kern_ipc_perm *msq, int msqflg)
{
	if (r_sec_msg_queue_associate)
		return r_sec_msg_queue_associate(msq, msqflg);
	return 0;
}

int security_msg_queue_msgctl(struct kern_ipc_perm *msq, int cmd)
{
	if (r_sec_msg_queue_msgctl)
		return r_sec_msg_queue_msgctl(msq, cmd);
	return 0;
}

int security_msg_queue_msgsnd(struct kern_ipc_perm *msq, struct msg_msg *msg,
			      int msqflg)
{
	if (r_sec_msg_queue_msgsnd)
		return r_sec_msg_queue_msgsnd(msq, msg, msqflg);
	return 0;
}

int security_msg_queue_msgrcv(struct kern_ipc_perm *msq, struct msg_msg *msg,
			      struct task_struct *target, long type, int mode)
{
	if (r_sec_msg_queue_msgrcv)
		return r_sec_msg_queue_msgrcv(msq, msg, target, type, mode);
	return 0;
}

int security_sem_alloc(struct kern_ipc_perm *sma)
{
	if (r_sec_sem_alloc)
		return r_sec_sem_alloc(sma);
	return 0;
}

void security_sem_free(struct kern_ipc_perm *sma)
{
	if (r_sec_sem_free)
		r_sec_sem_free(sma);
}

int security_sem_associate(struct kern_ipc_perm *sma, int semflg)
{
	if (r_sec_sem_associate)
		return r_sec_sem_associate(sma, semflg);
	return 0;
}

int security_sem_semctl(struct kern_ipc_perm *sma, int cmd)
{
	if (r_sec_sem_semctl)
		return r_sec_sem_semctl(sma, cmd);
	return 0;
}

int security_sem_semop(struct kern_ipc_perm *sma, struct sembuf *sops,
		       unsigned nsops, int alter)
{
	if (r_sec_sem_semop)
		return r_sec_sem_semop(sma, sops, nsops, alter);
	return 0;
}

int security_shm_alloc(struct kern_ipc_perm *shp)
{
	if (r_sec_shm_alloc)
		return r_sec_shm_alloc(shp);
	return 0;
}

void security_shm_free(struct kern_ipc_perm *shp)
{
	if (r_sec_shm_free)
		r_sec_shm_free(shp);
}

int security_shm_associate(struct kern_ipc_perm *shp, int shmflg)
{
	if (r_sec_shm_associate)
		return r_sec_shm_associate(shp, shmflg);
	return 0;
}

int security_shm_shmctl(struct kern_ipc_perm *shp, int cmd)
{
	if (r_sec_shm_shmctl)
		return r_sec_shm_shmctl(shp, cmd);
	return 0;
}

int security_shm_shmat(struct kern_ipc_perm *shp, char __user *shmaddr,
		       int shmflg)
{
	if (r_sec_shm_shmat)
		return r_sec_shm_shmat(shp, shmaddr, shmflg);
	return 0;
}
