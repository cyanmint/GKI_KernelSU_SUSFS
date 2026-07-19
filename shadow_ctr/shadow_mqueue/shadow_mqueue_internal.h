// SPDX-License-Identifier: GPL-2.0
#ifndef SHADOW_MQUEUE_INTERNAL_H
#define SHADOW_MQUEUE_INTERNAL_H

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/refcount.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/err.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/mount.h>
#include <linux/fcntl.h>
#include <uapi/linux/mount.h>
#include <linux/user_namespace.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#include <linux/mnt_idmapping.h>
#endif
#include <uapi/linux/mqueue.h>
#include <uapi/linux/time_types.h>

#include "shadow_hook.h"
#include "shadow_ctr_compat.h"
#include "include/uapi/shadow_mqueue.h"

#define SHADOW_MQUEUE_VERSION "2.0"
#define SHADOW_MQ_NAME_HTBITS	8	/* 256 name-hash buckets */
#define SHADOW_MQ_MOUNT_FSTYPE_MAX 32
#define SHADOW_MQ_MOUNT_TYPE_ARG   2
#define SHADOW_MQ_DEV_MQUEUE_PATH "/dev/mqueue"
#define SHADOW_MQ_DEV_MQUEUE_MODE 0755

/*
 * struct mq_msg - a single message stored in a queue.
 * Messages are kept in @msgs sorted by priority (highest first).
 */
struct mq_msg {
	struct list_head node;
	u32		 prio;
	u32		 len;
	u8		 data[]; /* flexible array; allocated with GFP_KERNEL */
};

/*
 * struct shadow_mq - a named message queue.
 *
 * Refcounted.  The name hash holds one reference.  Each open handle holds one
 * additional reference.  When unlinked the queue is removed from the hash; the
 * last handle close (or the name-hash drop if no handles are open) frees it.
 *
 * @msgs_lock: spinlock protecting @msgs and @curmsgs (needed for wait_event).
 * @wait_recv: blocked receivers wait here.
 * @wait_send: blocked senders wait here.
 */
struct shadow_mq {
	char			name[SHADOW_MQ_NAME_MAX + 1];
	s64			mq_maxmsg;
	s64			mq_msgsize;
	atomic_t		curmsgs;
	bool			nonblock;
	bool			unlinked;
	spinlock_t		msgs_lock;
	struct list_head	msgs;
	wait_queue_head_t	wait_recv;
	wait_queue_head_t	wait_send;
	refcount_t		refcount;
	struct hlist_node	name_node;
};

/* Per-open mq descriptor backing the anon-inode fd returned by mq_open(). */
struct mq_handle_entry {
	struct shadow_mq *mq;
	u32		  oflag;
	refcount_t	  refcount;
};

enum mq_wait_mode {
	MQ_WAIT_NONBLOCK,
	MQ_WAIT_INFINITE,
	MQ_WAIT_RELATIVE,
	MQ_WAIT_EXPIRED,
};

struct mq_wait_spec {
	enum mq_wait_mode mode;
	u64 timeout_ns;
};

#if defined(CONFIG_ARM64)
#define SHADOW_SYSCALL_ARG(_regs, _n) ((_regs)->regs[_n])
#define SHADOW_SYSCALL_SET_ARG(_regs, _n, _val) ((_regs)->regs[_n] = (_val))
#elif defined(CONFIG_X86_64)
static __always_inline unsigned long shadow_syscall_arg(const struct pt_regs *regs,
								 unsigned int n)
{
	switch (n) {
	case 0: return regs->di;
	case 1: return regs->si;
	case 2: return regs->dx;
	case 3: return regs->r10;
	case 4: return regs->r8;
	case 5: return regs->r9;
	default: return 0;
	}
}
#define SHADOW_SYSCALL_ARG(_regs, _n) shadow_syscall_arg((_regs), (_n))
static __always_inline void shadow_syscall_set_arg(struct pt_regs *regs,
						   unsigned int n,
						   unsigned long val)
{
	switch (n) {
	case 0: regs->di = val; break;
	case 1: regs->si = val; break;
	case 2: regs->dx = val; break;
	case 3: regs->r10 = val; break;
	case 4: regs->r8 = val; break;
	case 5: regs->r9 = val; break;
	}
}
#define SHADOW_SYSCALL_SET_ARG(_regs, _n, _val) \
	shadow_syscall_set_arg((_regs), (_n), (_val))
#else
#error "shadow_mqueue: unsupported architecture"
#endif

u32 mq_posix_to_shadow_oflag(int oflag);
int mq_copy_name_from_user(const char __user *uname, char *name);
int mq_wait_spec_from_abs_timeout(struct mq_wait_spec *wait, u32 oflag,
				  const struct __kernel_timespec __user *uabs);
void mq_fill_posix_attr_from_shadow(const struct shadow_mq_attr *shadow,
					 struct mq_attr *attr);
void mq_put(struct shadow_mq *mq);
long mq_do_unlink(const char *name);
long mq_do_open(const char *name, u32 oflag,
		const struct shadow_mq_attr *create_attr,
		struct mq_handle_entry **out_he,
		struct shadow_mq_attr *out_attr,
		bool *created_out);
long mq_do_getattr(struct mq_handle_entry *he, struct shadow_mq_attr *attr);
long mq_do_setattr(struct mq_handle_entry *he,
		   const struct shadow_mq_attr *newattr,
		   struct shadow_mq_attr *oldattr);
long mq_do_send(struct mq_handle_entry *he, const void *msg_data, u32 msg_len,
		u32 prio, const struct mq_wait_spec *wait);
long mq_do_receive(struct mq_handle_entry *he, void *msg_data, u32 *msg_len,
		   u32 *prio, const struct mq_wait_spec *wait);
bool mq_handle_get(struct mq_handle_entry *he);
void mq_handle_put(struct mq_handle_entry *he);
struct mq_handle_entry *mq_get_shadow_handle_from_fd(int mqdes);
int mq_create_anon_fd(const char *name, struct mq_handle_entry *he, int oflag,
		      bool created);
void mq_release_all(void);
int shadow_mqueue_install_hooks(void);
void shadow_mqueue_remove_hooks(void);

#endif
