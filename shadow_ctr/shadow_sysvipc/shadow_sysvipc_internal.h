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

#define SHADOW_SYSVIPC_VERSION "2.0"
#define SHADOW_SYSVIPC_MAX_RESOURCES	65536
#define SHADOW_SYSVIPC_KEY_HTBITS	8	/* 256 buckets */
#define SHADOW_SYSVIPC_TGID_HTBITS	8	/* 256 buckets */

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
 */
struct svipc_resource {
	u32			id;
	u32			type;
	s32			key;
	u32			flags;
	u32			nsems;
	u32			_pad;
	u64			size;
	refcount_t		refcount;
	struct hlist_node	key_node;
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

#endif
