// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_mqueue - simulated POSIX message queue subsystem
 *
 * A standalone loadable kernel module that provides fully functional POSIX
 * message queues on kernels built without CONFIG_POSIX_MQUEUE.
 *
 * Motivation
 * ----------
 * The native POSIX mqueue subsystem (CONFIG_POSIX_MQUEUE) is compiled into
 * vmlinux and mounts the mqueue filesystem.  It cannot be added by a module
 * after the kernel is built.
 *
 * runc uses POSIX message queues for parent↔child synchronisation during
 * container initialisation (the "init pipe").  Without CONFIG_POSIX_MQUEUE
 * that synchronisation fails and containers do not start.
 *
 * shadow_mqueue provides a drop-in replacement: stock userspace can call the
 * real mq_* syscalls and be transparently redirected into this module through
 * ftrace hooks. Message transfer is fully functional: bytes written by a
 * sender are buffered in a kernel-side priority-ordered list and delivered to
 * the receiver, with blocking and timeout semantics mirroring the POSIX
 * standard.
 *
 * Architecture
 * ------------
 * - Named queues persist in a global hash table keyed by name until unlinked.
 * - Transparent mq_open() returns a real anon-inode fd whose private_data is a
 *   refcounted mq_handle_entry.
 * - Messages are stored as heap-allocated mq_msg objects in a priority list.
 * - Receivers block on a wait_queue_head_t; senders wake them on each enqueue.
 *   Senders similarly block when a queue is full and are woken on dequeue.
 * - All resources (handles → queues → messages) are freed on fd close, so a
 *   crashed runtime never leaks kernel memory.
 */

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
#include <linux/fs_context.h>
#include <linux/magic.h>
#include <uapi/linux/mqueue.h>
#include <uapi/linux/time_types.h>

#include "shadow_hook.h"
#include "shadow_ctr_compat.h"
#include "include/uapi/shadow_mqueue.h"

#define SHADOW_MQUEUE_VERSION "2.0"

#define SHADOW_MQ_NAME_HTBITS	8	/* 256 name-hash buckets */

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

/* Global name registry. */
static DEFINE_HASHTABLE(mq_name_hash, SHADOW_MQ_NAME_HTBITS);
static DEFINE_MUTEX(mq_name_lock);

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

static long (*real_sys_mq_open)(const struct pt_regs *regs);
static long (*real_sys_mq_unlink)(const struct pt_regs *regs);
static long (*real_sys_mq_timedsend)(const struct pt_regs *regs);
static long (*real_sys_mq_timedreceive)(const struct pt_regs *regs);
static long (*real_sys_mq_notify)(const struct pt_regs *regs);
static long (*real_sys_mq_getsetattr)(const struct pt_regs *regs);

#if defined(CONFIG_ARM64)
#define SHADOW_SYSCALL_ARG(_regs, _n) ((_regs)->regs[_n])
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
#else
#error "shadow_mqueue: unsupported architecture"
#endif

static void mq_enqueue_locked(struct shadow_mq *mq, struct mq_msg *m);

static u32 mq_name_hash_val(const char *name)
{
	u32 h = 0;

	while (*name)
		h = h * 31 + (u8)*name++;
	return h;
}

/*
 * Look up a named queue.  Caller must hold mq_name_lock.
 * Returns a borrowed pointer; caller must bump refcount before releasing lock.
 */
static struct shadow_mq *mq_find_locked(const char *name)
{
	struct shadow_mq *mq;
	u32 h = mq_name_hash_val(name);

	hash_for_each_possible(mq_name_hash, mq, name_node, h) {
		if (!strcmp(mq->name, name))
			return mq;
	}
	return NULL;
}

static void mq_get(struct shadow_mq *mq)
{
	refcount_inc(&mq->refcount);
}

static void mq_put(struct shadow_mq *mq)
{
	struct mq_msg *msg, *tmp;

	if (!mq)
		return;
	if (!refcount_dec_and_test(&mq->refcount))
		return;

	/* Free all buffered messages. */
	list_for_each_entry_safe(msg, tmp, &mq->msgs, node) {
		list_del(&msg->node);
		kfree(msg);
	}
	kfree(mq);
}

static void mq_handle_put(struct mq_handle_entry *he)
{
	if (!he)
		return;
	if (!refcount_dec_and_test(&he->refcount))
		return;
	mq_put(he->mq);
	kfree(he);
}

static bool mq_handle_get(struct mq_handle_entry *he)
{
	return he && refcount_inc_not_zero(&he->refcount);
}

static void mq_fill_shadow_attr(struct shadow_mq *mq, struct shadow_mq_attr *attr)
{
	attr->mq_flags   = READ_ONCE(mq->nonblock) ? SHADOW_MQ_O_NONBLOCK : 0;
	attr->mq_maxmsg  = mq->mq_maxmsg;
	attr->mq_msgsize = mq->mq_msgsize;
	attr->mq_curmsgs = (s64)atomic_read(&mq->curmsgs);
}

static void mq_fill_posix_attr_from_shadow(const struct shadow_mq_attr *shadow,
					   struct mq_attr *attr)
{
	memset(attr, 0, sizeof(*attr));
	attr->mq_flags   = (shadow->mq_flags & SHADOW_MQ_O_NONBLOCK) ? O_NONBLOCK : 0;
	attr->mq_maxmsg  = shadow->mq_maxmsg;
	attr->mq_msgsize = shadow->mq_msgsize;
	attr->mq_curmsgs = shadow->mq_curmsgs;
}

static void mq_clamp_shadow_attr(struct shadow_mq_attr *attr)
{
	if (attr->mq_maxmsg <= 0 || attr->mq_maxmsg > SHADOW_MQ_MAXMSG_MAX)
		attr->mq_maxmsg = SHADOW_MQ_MAXMSG_DEF;
	if (attr->mq_msgsize <= 0 || attr->mq_msgsize > SHADOW_MQ_MSGSIZE_MAX)
		attr->mq_msgsize = SHADOW_MQ_MSGSIZE_MAX;
}

static u32 mq_posix_to_shadow_oflag(int oflag)
{
	u32 shadow = 0;

	if (oflag & O_CREAT)
		shadow |= SHADOW_MQ_O_CREAT;
	if (oflag & O_EXCL)
		shadow |= SHADOW_MQ_O_EXCL;
	if (oflag & O_NONBLOCK)
		shadow |= SHADOW_MQ_O_NONBLOCK;

	switch (oflag & O_ACCMODE) {
	case O_WRONLY:
		shadow |= SHADOW_MQ_O_WRONLY;
		break;
	case O_RDWR:
		shadow |= SHADOW_MQ_O_RDWR;
		break;
	case O_RDONLY:
	default:
		shadow |= SHADOW_MQ_O_RDONLY;
		break;
	}

	return shadow;
}

static int mq_copy_name_from_user(const char __user *uname, char *name)
{
	long copied;

	copied = strncpy_from_user(name, uname, SHADOW_MQ_NAME_MAX + 1);
	if (copied < 0)
		return copied;
	if (copied == 0)
		return -EINVAL;
	if (copied > SHADOW_MQ_NAME_MAX)
		return -ENAMETOOLONG;
	if (name[0] != '/')
		return -EINVAL;
	return 0;
}

static int mq_wait_spec_from_abs_timeout(struct mq_wait_spec *wait, u32 oflag,
					 const struct __kernel_timespec __user *uabs)
{
	struct __kernel_timespec uts;
	struct timespec64 now, abs;
	s64 delta_ns;

	if (oflag & SHADOW_MQ_O_NONBLOCK) {
		wait->mode = MQ_WAIT_NONBLOCK;
		wait->timeout_ns = 0;
		return 0;
	}
	if (!uabs) {
		wait->mode = MQ_WAIT_INFINITE;
		wait->timeout_ns = U64_MAX;
		return 0;
	}
	if (copy_from_user(&uts, uabs, sizeof(uts)))
		return -EFAULT;
	if (uts.tv_sec < 0 || uts.tv_nsec < 0 || uts.tv_nsec >= NSEC_PER_SEC)
		return -EINVAL;

	abs.tv_sec = uts.tv_sec;
	abs.tv_nsec = uts.tv_nsec;
	ktime_get_real_ts64(&now);
	delta_ns = timespec64_to_ns(&abs) - timespec64_to_ns(&now);
	if (delta_ns <= 0) {
		wait->mode = MQ_WAIT_EXPIRED;
		wait->timeout_ns = 0;
		return 0;
	}

	wait->mode = MQ_WAIT_RELATIVE;
	wait->timeout_ns = (u64)delta_ns;
	return 0;
}

static void mq_wake_waiters(struct shadow_mq *mq)
{
	wake_up_all(&mq->wait_send);
	wake_up_all(&mq->wait_recv);
}

static long mq_do_unlink(const char *name)
{
	struct shadow_mq *mq;

	mutex_lock(&mq_name_lock);
	mq = mq_find_locked(name);
	if (!mq) {
		mutex_unlock(&mq_name_lock);
		return -ENOENT;
	}
	hash_del(&mq->name_node);
	WRITE_ONCE(mq->unlinked, true);
	mutex_unlock(&mq_name_lock);

	mq_wake_waiters(mq);
	mq_put(mq);
	return 0;
}

static long mq_do_open(const char *name, u32 oflag,
		       const struct shadow_mq_attr *create_attr,
		       struct mq_handle_entry **out_he,
		       struct shadow_mq_attr *out_attr,
		       bool *created_out)
{
	struct shadow_mq_attr attr = {
		.mq_maxmsg = SHADOW_MQ_MAXMSG_DEF,
		.mq_msgsize = SHADOW_MQ_MSGSIZE_MAX,
	};
	struct shadow_mq *mq = NULL;
	struct mq_handle_entry *he;
	bool created = false;

	if (!name || !name[0] || name[0] != '/')
		return -EINVAL;

	if (create_attr)
		attr = *create_attr;
	mq_clamp_shadow_attr(&attr);

	mutex_lock(&mq_name_lock);
	mq = mq_find_locked(name);
	if (mq) {
		if ((oflag & SHADOW_MQ_O_CREAT) && (oflag & SHADOW_MQ_O_EXCL)) {
			mutex_unlock(&mq_name_lock);
			return -EEXIST;
		}
		if (READ_ONCE(mq->unlinked)) {
			mutex_unlock(&mq_name_lock);
			return -ENOENT;
		}
		mq_get(mq);
	} else {
		if (!(oflag & SHADOW_MQ_O_CREAT)) {
			mutex_unlock(&mq_name_lock);
			return -ENOENT;
		}
		mq = kzalloc(sizeof(*mq), GFP_KERNEL);
		if (!mq) {
			mutex_unlock(&mq_name_lock);
			return -ENOMEM;
		}
		strscpy(mq->name, name, sizeof(mq->name));
		mq->mq_maxmsg  = attr.mq_maxmsg;
		mq->mq_msgsize = attr.mq_msgsize;
		mq->nonblock   = !!(oflag & SHADOW_MQ_O_NONBLOCK);
		atomic_set(&mq->curmsgs, 0);
		spin_lock_init(&mq->msgs_lock);
		INIT_LIST_HEAD(&mq->msgs);
		init_waitqueue_head(&mq->wait_recv);
		init_waitqueue_head(&mq->wait_send);
		refcount_set(&mq->refcount, 2);
		hash_add(mq_name_hash, &mq->name_node, mq_name_hash_val(name));
		created = true;
	}
	mutex_unlock(&mq_name_lock);

	he = kzalloc(sizeof(*he), GFP_KERNEL);
	if (!he) {
		if (created)
			mq_do_unlink(name);
		mq_put(mq);
		return -ENOMEM;
	}

	he->mq = mq;
	he->oflag = oflag;
	refcount_set(&he->refcount, 1);

	if (out_attr)
		mq_fill_shadow_attr(mq, out_attr);
	if (created_out)
		*created_out = created;
	*out_he = he;
	return 0;
}

static long mq_do_getattr(struct mq_handle_entry *he, struct shadow_mq_attr *attr)
{
	if (!he || !attr)
		return -EINVAL;

	mq_fill_shadow_attr(he->mq, attr);
	return 0;
}

static long mq_do_setattr(struct mq_handle_entry *he,
			  const struct shadow_mq_attr *newattr,
			  struct shadow_mq_attr *oldattr)
{
	u32 oflag;

	if (!he)
		return -EINVAL;

	if (oldattr)
		mq_fill_shadow_attr(he->mq, oldattr);
	if (!newattr)
		return 0;

	WRITE_ONCE(he->mq->nonblock, !!(newattr->mq_flags & SHADOW_MQ_O_NONBLOCK));
	oflag = READ_ONCE(he->oflag);
	if (READ_ONCE(he->mq->nonblock))
		oflag |= SHADOW_MQ_O_NONBLOCK;
	else
		oflag &= ~SHADOW_MQ_O_NONBLOCK;
	WRITE_ONCE(he->oflag, oflag);
	return 0;
}

static long mq_do_send(struct mq_handle_entry *he, const void *msg_data, u32 msg_len,
		       u32 prio, const struct mq_wait_spec *wait)
{
	struct shadow_mq *mq;
	struct mq_msg *msg;
	long ret = 0;

	if (!he || !msg_data || !wait)
		return -EINVAL;

	mq = he->mq;
	if (msg_len == 0 || (s64)msg_len > mq->mq_msgsize)
		return -EINVAL;

	msg = kmalloc(sizeof(*msg) + msg_len, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->prio = prio;
	msg->len = msg_len;
	memcpy(msg->data, msg_data, msg_len);

	for (;;) {
		spin_lock(&mq->msgs_lock);
		if (atomic_read(&mq->curmsgs) < (int)mq->mq_maxmsg) {
			mq_enqueue_locked(mq, msg);
			spin_unlock(&mq->msgs_lock);
			wake_up(&mq->wait_recv);
			return 0;
		}
		spin_unlock(&mq->msgs_lock);

		if (READ_ONCE(mq->unlinked)) {
			ret = -ENOENT;
			break;
		}

		switch (wait->mode) {
		case MQ_WAIT_NONBLOCK:
			ret = -EAGAIN;
			break;
		case MQ_WAIT_EXPIRED:
			ret = -ETIMEDOUT;
			break;
		case MQ_WAIT_INFINITE:
			ret = wait_event_interruptible(mq->wait_send,
				atomic_read(&mq->curmsgs) < (int)mq->mq_maxmsg ||
				READ_ONCE(mq->unlinked));
			if (ret < 0)
				ret = -EINTR;
			else
				ret = 0;
			break;
		case MQ_WAIT_RELATIVE:
			ret = wait_event_interruptible_timeout(mq->wait_send,
				atomic_read(&mq->curmsgs) < (int)mq->mq_maxmsg ||
				READ_ONCE(mq->unlinked),
				nsecs_to_jiffies(wait->timeout_ns) + 1);
			if (ret == 0)
				ret = -ETIMEDOUT;
			else if (ret < 0)
				ret = -EINTR;
			else
				ret = 0;
			break;
		}
		if (ret)
			break;
	}

	kfree(msg);
	return ret;
}

static long mq_do_receive(struct mq_handle_entry *he, void *msg_data, u32 *msg_len,
			  u32 *prio, const struct mq_wait_spec *wait)
{
	struct shadow_mq *mq;
	struct mq_msg *msg = NULL;
	long ret = 0;

	if (!he || !msg_data || !msg_len || !wait)
		return -EINVAL;

	mq = he->mq;

	for (;;) {
		spin_lock(&mq->msgs_lock);
		if (!list_empty(&mq->msgs)) {
			msg = list_first_entry(&mq->msgs, struct mq_msg, node);
			list_del(&msg->node);
			atomic_dec(&mq->curmsgs);
			spin_unlock(&mq->msgs_lock);
			wake_up(&mq->wait_send);
			break;
		}
		spin_unlock(&mq->msgs_lock);

		if (READ_ONCE(mq->unlinked))
			return -ENOENT;

		switch (wait->mode) {
		case MQ_WAIT_NONBLOCK:
			return -EAGAIN;
		case MQ_WAIT_EXPIRED:
			return -ETIMEDOUT;
		case MQ_WAIT_INFINITE:
			ret = wait_event_interruptible(mq->wait_recv,
				atomic_read(&mq->curmsgs) > 0 ||
				READ_ONCE(mq->unlinked));
			if (ret < 0)
				return -EINTR;
			break;
		case MQ_WAIT_RELATIVE:
			ret = wait_event_interruptible_timeout(mq->wait_recv,
				atomic_read(&mq->curmsgs) > 0 ||
				READ_ONCE(mq->unlinked),
				nsecs_to_jiffies(wait->timeout_ns) + 1);
			if (ret == 0)
				return -ETIMEDOUT;
			if (ret < 0)
				return -EINTR;
			break;
		}
	}

	if (msg->len > *msg_len) {
		kfree(msg);
		return -EMSGSIZE;
	}

	if (prio)
		*prio = msg->prio;
	*msg_len = msg->len;
	memcpy(msg_data, msg->data, msg->len);
	kfree(msg);
	return 0;
}

/*
 * Enqueue a message in priority order (highest prio first).
 * Caller must hold mq->msgs_lock.
 */
static void mq_enqueue_locked(struct shadow_mq *mq, struct mq_msg *m)
{
	struct mq_msg *cur;

	list_for_each_entry(cur, &mq->msgs, node) {
		if (m->prio > cur->prio) {
			list_add_tail(&m->node, &cur->node);
			atomic_inc(&mq->curmsgs);
			return;
		}
	}
	list_add_tail(&m->node, &mq->msgs);
	atomic_inc(&mq->curmsgs);
}

static int shadow_mq_anon_release(struct inode *inode, struct file *file)
{
	struct mq_handle_entry *he = file->private_data;

	file->private_data = NULL;
	mq_handle_put(he);
	return 0;
}

static const struct file_operations shadow_mq_anon_fops = {
	.owner		= THIS_MODULE,
	.release	= shadow_mq_anon_release,
	.llseek		= noop_llseek,
};

static struct mq_handle_entry *mq_get_shadow_handle_from_fd(int mqdes)
{
	struct mq_handle_entry *he;
	struct fd f = fdget(mqdes);

	if (fd_empty(f))
		return ERR_PTR(-EBADF);
	if (fd_file(f)->f_op != &shadow_mq_anon_fops) {
		fdput(f);
		return NULL;
	}

	he = fd_file(f)->private_data;
	if (!mq_handle_get(he)) {
		fdput(f);
		return ERR_PTR(-EBADF);
	}
	fdput(f);
	return he;
}

static long hook_sys_mq_open(const struct pt_regs *regs)
{
	const char __user *uname = (const char __user *)SHADOW_SYSCALL_ARG(regs, 0);
	int oflag = (int)SHADOW_SYSCALL_ARG(regs, 1);
	struct mq_attr __user *uattr =
		(struct mq_attr __user *)SHADOW_SYSCALL_ARG(regs, 3);
	struct shadow_mq_attr create_attr = {
		.mq_maxmsg = SHADOW_MQ_MAXMSG_DEF,
		.mq_msgsize = SHADOW_MQ_MSGSIZE_MAX,
	};
	struct mq_handle_entry *he;
	struct mq_attr attr;
	char name[SHADOW_MQ_NAME_MAX + 1];
	bool created = false;
	long ret;
	int fd;

	ret = real_sys_mq_open(regs);
	if (ret != -ENOSYS)
		return ret;

	ret = mq_copy_name_from_user(uname, name);
	if (ret)
		return ret;

	if ((oflag & O_CREAT) && uattr) {
		if (copy_from_user(&attr, uattr, sizeof(attr)))
			return -EFAULT;
		create_attr.mq_maxmsg = attr.mq_maxmsg;
		create_attr.mq_msgsize = attr.mq_msgsize;
	}

	ret = mq_do_open(name, mq_posix_to_shadow_oflag(oflag), &create_attr, &he,
			 NULL, &created);
	if (ret)
		return ret;

	fd = anon_inode_getfd("shadow_mqueue", &shadow_mq_anon_fops, he,
			      O_RDWR | (oflag & O_CLOEXEC));
	if (fd < 0) {
		if (created)
			mq_do_unlink(name);
		mq_handle_put(he);
		return fd;
	}

	return fd;
}

static long hook_sys_mq_unlink(const struct pt_regs *regs)
{
	const char __user *uname = (const char __user *)SHADOW_SYSCALL_ARG(regs, 0);
	char name[SHADOW_MQ_NAME_MAX + 1];
	long ret;

	ret = real_sys_mq_unlink(regs);
	if (ret != -ENOSYS)
		return ret;

	ret = mq_copy_name_from_user(uname, name);
	if (ret)
		return ret;

	return mq_do_unlink(name);
}

static long hook_sys_mq_timedsend(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)SHADOW_SYSCALL_ARG(regs, 0);
	const char __user *umsg = (const char __user *)SHADOW_SYSCALL_ARG(regs, 1);
	size_t msg_len = (size_t)SHADOW_SYSCALL_ARG(regs, 2);
	unsigned int prio = (unsigned int)SHADOW_SYSCALL_ARG(regs, 3);
	const struct __kernel_timespec __user *uabs =
		(const struct __kernel_timespec __user *)SHADOW_SYSCALL_ARG(regs, 4);
	struct mq_handle_entry *he;
	struct mq_wait_spec wait;
	u8 msg_data[SHADOW_MQ_MSGSIZE_MAX];
	long ret;

	he = mq_get_shadow_handle_from_fd(mqdes);
	if (IS_ERR(he))
		return PTR_ERR(he);
	if (!he)
		return real_sys_mq_timedsend(regs);

	if (msg_len > sizeof(msg_data)) {
		mq_handle_put(he);
		return -EMSGSIZE;
	}
	if (copy_from_user(msg_data, umsg, msg_len)) {
		mq_handle_put(he);
		return -EFAULT;
	}

	ret = mq_wait_spec_from_abs_timeout(&wait, READ_ONCE(he->oflag), uabs);
	if (!ret)
		ret = mq_do_send(he, msg_data, (u32)msg_len, prio, &wait);
	mq_handle_put(he);
	return ret;
}

static long hook_sys_mq_timedreceive(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)SHADOW_SYSCALL_ARG(regs, 0);
	char __user *umsg = (char __user *)SHADOW_SYSCALL_ARG(regs, 1);
	size_t msg_len = (size_t)SHADOW_SYSCALL_ARG(regs, 2);
	unsigned int __user *uprio =
		(unsigned int __user *)SHADOW_SYSCALL_ARG(regs, 3);
	const struct __kernel_timespec __user *uabs =
		(const struct __kernel_timespec __user *)SHADOW_SYSCALL_ARG(regs, 4);
	struct mq_handle_entry *he;
	struct mq_wait_spec wait;
	u8 msg_data[SHADOW_MQ_MSGSIZE_MAX];
	u32 len;
	u32 prio;
	long ret;

	he = mq_get_shadow_handle_from_fd(mqdes);
	if (IS_ERR(he))
		return PTR_ERR(he);
	if (!he)
		return real_sys_mq_timedreceive(regs);

	if (msg_len > sizeof(msg_data)) {
		mq_handle_put(he);
		return -EMSGSIZE;
	}

	ret = mq_wait_spec_from_abs_timeout(&wait, READ_ONCE(he->oflag), uabs);
	if (ret)
		goto out_put;

	len = (u32)msg_len;
	ret = mq_do_receive(he, msg_data, &len, &prio, &wait);
	if (ret)
		goto out_put;

	if (copy_to_user(umsg, msg_data, len)) {
		ret = -EFAULT;
		goto out_put;
	}
	if (uprio && put_user(prio, uprio)) {
		ret = -EFAULT;
		goto out_put;
	}

	ret = len;
out_put:
	mq_handle_put(he);
	return ret;
}

static long hook_sys_mq_notify(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)SHADOW_SYSCALL_ARG(regs, 0);
	struct mq_handle_entry *he;

	he = mq_get_shadow_handle_from_fd(mqdes);
	if (IS_ERR(he))
		return PTR_ERR(he);
	if (!he)
		return real_sys_mq_notify(regs);

	mq_handle_put(he);
	return -ENOSYS;
}

static long hook_sys_mq_getsetattr(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)SHADOW_SYSCALL_ARG(regs, 0);
	const struct mq_attr __user *unew =
		(const struct mq_attr __user *)SHADOW_SYSCALL_ARG(regs, 1);
	struct mq_attr __user *uold =
		(struct mq_attr __user *)SHADOW_SYSCALL_ARG(regs, 2);
	struct mq_handle_entry *he;
	struct shadow_mq_attr newattr;
	struct shadow_mq_attr oldattr;
	struct mq_attr old;
	long ret;

	he = mq_get_shadow_handle_from_fd(mqdes);
	if (IS_ERR(he))
		return PTR_ERR(he);
	if (!he)
		return real_sys_mq_getsetattr(regs);

	if (unew) {
		if (copy_from_user(&old, unew, sizeof(old))) {
			ret = -EFAULT;
			goto out_put;
		}
		memset(&newattr, 0, sizeof(newattr));
		newattr.mq_flags = (old.mq_flags & O_NONBLOCK) ? SHADOW_MQ_O_NONBLOCK : 0;
		ret = mq_do_setattr(he, &newattr, uold ? &oldattr : NULL);
	} else {
		ret = mq_do_getattr(he, &oldattr);
	}
	if (ret)
		goto out_put;

	if (uold) {
		mq_fill_posix_attr_from_shadow(&oldattr, &old);
		if (copy_to_user(uold, &old, sizeof(old))) {
			ret = -EFAULT;
			goto out_put;
		}
	}

	ret = 0;
out_put:
	mq_handle_put(he);
	return ret;
}

static const char * const mq_open_hook_names[] = {
	"__arm64_sys_mq_open",
	"sys_mq_open",
	NULL,
};

static const char * const mq_unlink_hook_names[] = {
	"__arm64_sys_mq_unlink",
	"sys_mq_unlink",
	NULL,
};

static const char * const mq_timedsend_hook_names[] = {
	"__arm64_sys_mq_timedsend",
	"sys_mq_timedsend",
	NULL,
};

static const char * const mq_timedreceive_hook_names[] = {
	"__arm64_sys_mq_timedreceive",
	"sys_mq_timedreceive",
	NULL,
};

static const char * const mq_notify_hook_names[] = {
	"__arm64_sys_mq_notify",
	"sys_mq_notify",
	NULL,
};

static const char * const mq_getsetattr_hook_names[] = {
	"__arm64_sys_mq_getsetattr",
	"sys_mq_getsetattr",
	NULL,
};

static struct shadow_hook mq_open_hook =
	SHADOW_HOOK(mq_open_hook_names, hook_sys_mq_open, &real_sys_mq_open);
static struct shadow_hook mq_unlink_hook =
	SHADOW_HOOK(mq_unlink_hook_names, hook_sys_mq_unlink, &real_sys_mq_unlink);
static struct shadow_hook mq_timedsend_hook =
	SHADOW_HOOK(mq_timedsend_hook_names, hook_sys_mq_timedsend,
		    &real_sys_mq_timedsend);
static struct shadow_hook mq_timedreceive_hook =
	SHADOW_HOOK(mq_timedreceive_hook_names, hook_sys_mq_timedreceive,
		    &real_sys_mq_timedreceive);
static struct shadow_hook mq_notify_hook =
	SHADOW_HOOK(mq_notify_hook_names, hook_sys_mq_notify, &real_sys_mq_notify);
static struct shadow_hook mq_getsetattr_hook =
	SHADOW_HOOK(mq_getsetattr_hook_names, hook_sys_mq_getsetattr,
		    &real_sys_mq_getsetattr);

static struct shadow_hook *shadow_mqueue_hooks[] = {
	&mq_open_hook,
	&mq_unlink_hook,
	&mq_timedsend_hook,
	&mq_timedreceive_hook,
	&mq_notify_hook,
	&mq_getsetattr_hook,
	NULL,
};

/*
 * shadow_mqueue_fs_type - minimal pseudo filesystem registered under the
 * name "mqueue".
 *
 * Container runtimes such as runc unconditionally attempt to
 * `mount("mqueue", "<rootfs>/dev/mqueue", "mqueue", ...)` during container
 * init, independent of whether the workload actually uses POSIX message
 * queues. On a kernel built without CONFIG_POSIX_MQUEUE there is no
 * filesystem type named "mqueue" registered at all, so that mount(2) call
 * fails with -ENODEV ("no such device") and container start-up aborts
 * before shadow_mqueue's hooked mq_* syscalls ever get a chance to run.
 *
 * The mq_* syscalls hooked above are fully self-contained (they resolve
 * queues through the global name hash and anon-inode fds, not through any
 * on-disk/vfs state), so this filesystem's contents are irrelevant to
 * message-queue semantics - it only needs to exist so the mount(2) call
 * that gates container startup succeeds. An empty, single-instance pseudo
 * fs (mirroring the shape of tmpfs/proc's fs_context-based mount, but with
 * no populated entries) is sufficient.
 */
/* Same value the real in-tree ipc/mqueue.c uses for its "mqueue" fs magic. */
#define SHADOW_MQUEUE_FS_MAGIC 0x19800202

static int shadow_mqueuefs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	return simple_fill_super(sb, SHADOW_MQUEUE_FS_MAGIC, NULL);
}

static int shadow_mqueuefs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, shadow_mqueuefs_fill_super);
}

static const struct fs_context_operations shadow_mqueuefs_context_ops = {
	.get_tree	= shadow_mqueuefs_get_tree,
};

static int shadow_mqueuefs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &shadow_mqueuefs_context_ops;
	return 0;
}

static struct file_system_type shadow_mqueue_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "mqueue",
	.init_fs_context = shadow_mqueuefs_init_fs_context,
	.kill_sb	= kill_litter_super,
	.fs_flags	= FS_USERNS_MOUNT,
};

static bool shadow_mqueue_fs_registered;

int __init shadow_mqueue_init(void)
{
	int ret;

	pr_info("shadow_mqueue: init: registering \"mqueue\" filesystem type\n");
	ret = register_filesystem(&shadow_mqueue_fs_type);
	if (ret == 0) {
		shadow_mqueue_fs_registered = true;
		pr_info("shadow_mqueue: init: \"mqueue\" filesystem type registered\n");
	} else if (ret == -EBUSY) {
		/*
		 * A filesystem named "mqueue" is already registered (e.g. the
		 * kernel was actually built with CONFIG_POSIX_MQUEUE, or
		 * another module raced us). That is not fatal: mount(2) of
		 * "mqueue" will already succeed via that registration.
		 */
		pr_info("shadow_mqueue: init: \"mqueue\" filesystem type already registered, skipping\n");
	} else {
		pr_err("shadow_mqueue: init: register_filesystem(\"mqueue\") failed: %d\n", ret);
		return ret;
	}

	pr_info("shadow_mqueue: init: installing transparent mq_* hooks\n");
	ret = shadow_hook_install_all(shadow_mqueue_hooks, "shadow_mqueue");
	if (ret < 0) {
		pr_err("shadow_mqueue: init: shadow_hook_install_all() failed: %d\n", ret);
		if (shadow_mqueue_fs_registered) {
			unregister_filesystem(&shadow_mqueue_fs_type);
			shadow_mqueue_fs_registered = false;
		}
		shadow_hook_remove_all(shadow_mqueue_hooks);
		return ret;
	}
	pr_info("shadow_mqueue: init: %d hook(s) installed\n", ret);

	pr_info("shadow_mqueue: simulated POSIX mqueue subsystem loaded with transparent mq_* hooks\n");
	return 0;
}

void shadow_mqueue_exit(void)
{
	struct shadow_mq *mq;
	struct hlist_node *tmp;
	int bkt;

	pr_info("shadow_mqueue: exit: removing transparent mq_* hooks\n");
	shadow_hook_remove_all(shadow_mqueue_hooks);
	if (shadow_mqueue_fs_registered) {
		pr_info("shadow_mqueue: exit: unregistering \"mqueue\" filesystem type\n");
		unregister_filesystem(&shadow_mqueue_fs_type);
		shadow_mqueue_fs_registered = false;
	}

	/*
	 * Wake any blocked waiters, mark queues unlinked, and drop the
	 * name-hash references.  The module must not be unloaded while
	 * processes still have the device open (rmmod would be refused by
	 * the kernel's module refcount in that case).
	 */
	mutex_lock(&mq_name_lock);
	hash_for_each_safe(mq_name_hash, bkt, tmp, mq, name_node) {
		hash_del(&mq->name_node);
		WRITE_ONCE(mq->unlinked, true);
		wake_up_all(&mq->wait_send);
		wake_up_all(&mq->wait_recv);
		mq_put(mq); /* name-hash reference */
	}
	mutex_unlock(&mq_name_lock);

	pr_info("shadow_mqueue: simulated POSIX mqueue subsystem unloaded\n");
}

/*
 * Presence marker for shadow_ctr_checker (see shadow_sysvipc.c for rationale).
 */
int shadow_mqueue_is_active(void)
{
	return 1;
}
EXPORT_SYMBOL_GPL(shadow_mqueue_is_active);

module_init(shadow_mqueue_init);
module_exit(shadow_mqueue_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Simulated POSIX message queue subsystem with transparent mq_* syscall hijacking and a \"mqueue\" filesystem type");
MODULE_VERSION(SHADOW_MQUEUE_VERSION);
