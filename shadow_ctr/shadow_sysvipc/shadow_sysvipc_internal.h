// SPDX-License-Identifier: GPL-2.0
#ifndef SHADOW_SYSVIPC_INTERNAL_H
#define SHADOW_SYSVIPC_INTERNAL_H

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <linux/refcount.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/hashtable.h>
#include <linux/wait.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/sem.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <uapi/linux/shm.h>
#include <asm/ptrace.h>

#include "shadow_hook.h"
#include "include/uapi/shadow_sysvipc.h"

/*
 * shadow_ns_current_ipc_ns_id() is implemented in ../shadow_ns/shadow_ns_task.c
 * and declared in ../shadow_ns/shadow_ns_internal.h; forward-declared here
 * instead of including that (much larger, PID/UTS/USER-namespace-private)
 * header, since shadow_sysvipc only needs this one function. Both
 * subsystems link into the same shadow_ctr.ko (see ../Makefile), so this
 * resolves at link time without EXPORT_SYMBOL/symbol_get.
 */
u32 shadow_ns_current_ipc_ns_id(void);

#define SHADOW_SYSVIPC_VERSION "2.0"
#define SHADOW_SYSVIPC_MAX_RESOURCES	65536
#define SHADOW_SYSVIPC_KEY_HTBITS	8	/* 256 buckets */
#define SHADOW_SYSVIPC_TGID_HTBITS	8	/* 256 buckets */

/* Per-queue message payload limits (mirrors the real ipc/msg.c defaults). */
#define SHADOW_SYSVIPC_MSG_MAXSIZE	MSGMAX
#define SHADOW_SYSVIPC_MSG_MAXQBYTES	MSGMNB
#define SHADOW_SYSVIPC_MSG_MAXCOUNT	MSGMNB

/*
 * struct svipc_msg - a single queued message payload (TYPE_MSGQ only).
 * @node:  linkage in svipc_resource.msgs, oldest-first
 * @mtype: message type as passed to msgsnd(2)
 * @len:   length of @data in bytes
 * @data:  flexible array holding the raw message payload
 */
struct svipc_msg {
	struct list_head	node;
	long			mtype;
	size_t			len;
	char			data[];
};

/*
 * struct svipc_resource - a single virtual IPC object.
 * @id:        stable id handed to userspace (xa key)
 * @type:      enum shadow_sysvipc_type
 * @key:       application key, or SHADOW_IPC_PRIVATE
 * @flags:     permission bits stored at creation
 * @nsems:     semaphore count (TYPE_SEM only)
 * @size:      segment size (TYPE_SHM only)
 * @refcount:  reference count; dropped by each owner reference
 * @key_node:  hash node in the global key table (only when key != PRIVATE)
 * @msgs:      queued svipc_msg list (TYPE_MSGQ only)
 * @msgs_lock: serialises @msgs, @msg_count and @msg_qbytes
 * @msgs_wait: waiters blocked in msgrcv(2) waiting for a matching message
 * @msg_count: number of messages currently queued
 * @msg_qbytes: total payload bytes currently queued
 * @sem_val:   per-semaphore value array (TYPE_SEM only, @nsems entries)
 * @sem_lock:  serialises @sem_val (semop(2)'s all-or-nothing multi-op apply)
 * @sem_wait:  waiters blocked in semop(2)/semtimedop(2) on an op that could
 *             not be satisfied immediately
 * @shm_file:  backing shmem file for the segment (TYPE_SHM only), created
 *             lazily by the first shmat(2); every subsequent shmat(2) of the
 *             same @id mmap()s the same file so attaches genuinely share
 *             memory, exactly like real SysV shared memory
 * @shm_lock:  serialises lazy @shm_file creation
 */
struct svipc_resource {
	u32			id;
	u32			type;
	s32			key;
	u32			flags;
	u32			nsems;
	u32			ns_id;
	u64			size;
	refcount_t		refcount;
	struct hlist_node	key_node;
	struct list_head	msgs;
	struct mutex		msgs_lock;
	wait_queue_head_t	msgs_wait;
	u32			msg_count;
	u64			msg_qbytes;
	int			*sem_val;
	struct mutex		sem_lock;
	wait_queue_head_t	sem_wait;
	struct file		*shm_file;
	struct mutex		shm_lock;
};

struct svipc_owned_ref {
	struct list_head	node;
	struct svipc_resource	*res;
};

/*
 * struct svipc_tgid_owner - per-task-group ownership state for hooked syscalls.
 * @tgid: thread-group id that owns @owned references
 * @owned: list of svipc_owned_ref entries held by this task group
 * @lock: serialises operations on @owned
 * @node: hash-table linkage
 *
 * Reaping is lazy rather than tracepoint-driven: on each transparent syscall
 * entry we opportunistically sweep the ownership table and drop entries whose
 * TGID no longer resolves to a live thread group.  This avoids sleeping/locking
 * concerns in sched_process_exit tracepoint context at the cost of leaked
 * bookkeeping surviving until the next intercepted SysV IPC syscall.
 */
struct svipc_tgid_owner {
	pid_t			 tgid;
	struct list_head	 owned;
	struct mutex		 lock;
	struct hlist_node	 node;
};

struct svipc_resource *svipc_get(u32 id);
void svipc_put(struct svipc_resource *res);
u32 svipc_shadow_flags_from_ipc(int flags);
int svipc_resource_create_or_get(u32 type, s32 key, u32 flags,
			 u32 nsems, u64 size,
			 struct svipc_resource **res_out);
int svipc_resource_stat(u32 type, u32 id, struct shadow_sysvipc_stat *stat);
void svipc_tgid_reap_dead(void);
int svipc_tgid_own_current(struct svipc_resource *res);
int svipc_tgid_destroy_current(u32 type, u32 id);
void svipc_tgid_release_all(void);
void svipc_force_free_all_resources(void);

/* shadow_sysvipc_msgq.c: simulated message-queue payload transfer. */
void svipc_msgq_purge_locked(struct svipc_resource *res);
long svipc_sys_msgsnd(int msqid, const void __user *umsgp, size_t msgsz,
		      int msgflg);
long svipc_sys_msgrcv(int msqid, void __user *umsgp, size_t msgsz,
		      long msgtyp, int msgflg);

/* shadow_sysvipc_sem.c: simulated semaphore operations (semop/semtimedop)
 * and semctl(2)'s value-manipulation commands (GETVAL/SETVAL/GETALL/SETALL).
 */
void svipc_sem_purge_locked(struct svipc_resource *res);
long svipc_sys_semop(int semid, struct sembuf __user *tsops, unsigned int nsops,
		    const struct __kernel_timespec __user *utimeout);
long svipc_sys_semctl_val(int semid, int semnum, int cmd, unsigned long arg);

/* shadow_sysvipc_shm.c: simulated shared-memory attach/detach (shmat/shmdt),
 * backed by a lazily-created shmem file shared by every attach of the same
 * segment id.
 */
void svipc_shm_purge_locked(struct svipc_resource *res);
long svipc_sys_shmat(int shmid, const void __user *ushmaddr, int shmflg,
		    unsigned long *raddr);
long svipc_sys_shmdt(const void __user *ushmaddr);

#endif
