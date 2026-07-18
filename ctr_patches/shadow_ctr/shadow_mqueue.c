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
 * ftrace hooks, while the older /dev/shadow_mqueue ioctl API remains available.
 * Message transfer is fully functional: bytes written by a sender are buffered
 * in a kernel-side priority-ordered list and delivered to the receiver, with
 * blocking and timeout semantics mirroring the POSIX standard.
 *
 * Architecture
 * ------------
 * - Named queues persist in a global hash table keyed by name until unlinked.
 * - Transparent mq_open() returns a real anon-inode fd whose private_data is a
 *   refcounted mq_handle_entry; ioctl open("/dev/shadow_mqueue") still creates
 *   a per-fd session with its own handle table.
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
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
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
#include <uapi/linux/mqueue.h>
#include <uapi/linux/time_types.h>

#include "shadow_hook.h"
#include "shadow_ctr_internal.h"
#include "include/uapi/shadow_mqueue.h"

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

/*
 * struct mq_handle_entry - per-session handle.
 * Maps a handle id to an mq + the open flags used when the handle was created.
 */
struct mq_handle_entry {
	struct shadow_mq *mq;
	u32		  oflag;
	refcount_t	  refcount;
};

/* Per-open-fd session. */
struct mq_session {
	struct xarray	handles; /* handle_id -> mq_handle_entry * */
	struct mutex	lock;
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

static void mq_wait_spec_from_ioctl(struct mq_wait_spec *wait, u32 oflag,
				    u64 timeout_ns)
{
	if (oflag & SHADOW_MQ_O_NONBLOCK || timeout_ns == 0) {
		wait->mode = MQ_WAIT_NONBLOCK;
		wait->timeout_ns = 0;
		return;
	}
	if (timeout_ns == U64_MAX) {
		wait->mode = MQ_WAIT_INFINITE;
		wait->timeout_ns = U64_MAX;
		return;
	}
	wait->mode = MQ_WAIT_RELATIVE;
	wait->timeout_ns = timeout_ns;
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

static struct mq_handle_entry *mq_session_lookup(struct mq_session *s, u32 handle)
{
	struct mq_handle_entry *he;

	mutex_lock(&s->lock);
	he = xa_load(&s->handles, handle);
	if (!mq_handle_get(he))
		he = NULL;
	mutex_unlock(&s->lock);
	return he;
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

/* ---- ioctl handlers ---- */

static long mqioc_open(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_open_req *req;
	struct mq_handle_entry *he;
	bool created = false;
	u32 handle;
	int ret;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(req, arg, sizeof(*req)))
		goto out_req;

	req->name[SHADOW_MQ_NAME_MAX] = '\0';
	ret = mq_do_open(req->name, req->oflag, &req->attr, &he, &req->attr,
			 &created);
	if (ret)
		goto out_req;

	mutex_lock(&s->lock);
	ret = xa_alloc(&s->handles, &handle, he, XA_LIMIT(1, INT_MAX),
		       GFP_KERNEL);
	mutex_unlock(&s->lock);
	if (ret) {
		mq_handle_put(he);
		goto out_req;
	}
	req->handle = handle;

	ret = copy_to_user(arg, req, sizeof(*req)) ? -EFAULT : 0;
	if (ret) {
		/* Failed to return the handle to userspace; undo allocation. */
		mutex_lock(&s->lock);
		xa_erase(&s->handles, handle);
		mutex_unlock(&s->lock);
		if (created)
			mq_do_unlink(he->mq->name);
		mq_handle_put(he);
	}
out_req:
	kfree(req);
	return ret;
}

static long mqioc_close(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_close_req req;
	struct mq_handle_entry *he;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&s->lock);
	he = xa_erase(&s->handles, req.handle);
	mutex_unlock(&s->lock);
	if (!he)
		return -EBADF;

	mq_handle_put(he);
	return 0;
}

static long mqioc_unlink(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_unlink_req req;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	req.name[SHADOW_MQ_NAME_MAX] = '\0';
	return mq_do_unlink(req.name);
}

static long mqioc_send(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_send_req *req;
	struct mq_handle_entry *he;
	struct mq_wait_spec wait;
	long ret = 0;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(req, arg, sizeof(*req)))
		goto out_req;

	ret = -EBADF;
	he = mq_session_lookup(s, req->handle);
	if (!he)
		goto out_req;

	mq_wait_spec_from_ioctl(&wait, READ_ONCE(he->oflag), req->timeout_ns);
	ret = mq_do_send(he, req->msg_data, req->msg_len, req->prio, &wait);
	mq_handle_put(he);
out_req:
	kfree(req);
	return ret;
}

static long mqioc_receive(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_recv_req *req;
	struct mq_handle_entry *he;
	struct mq_wait_spec wait;
	long ret = 0;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(req, arg, sizeof(*req)))
		goto out_req;

	ret = -EBADF;
	he = mq_session_lookup(s, req->handle);
	if (!he)
		goto out_req;

	mq_wait_spec_from_ioctl(&wait, READ_ONCE(he->oflag), req->timeout_ns);
	ret = mq_do_receive(he, req->msg_data, &req->msg_len, &req->prio, &wait);
	mq_handle_put(he);
	if (ret)
		goto out_req;

	if (copy_to_user(arg, req, sizeof(*req)))
		ret = -EFAULT;
out_req:
	kfree(req);
	return ret;
}

static long mqioc_getattr(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_attr_req req;
	struct mq_handle_entry *he;
	long ret;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	he = mq_session_lookup(s, req.handle);
	if (!he)
		return -EBADF;

	ret = mq_do_getattr(he, &req.attr);
	mq_handle_put(he);
	if (ret)
		return ret;

	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long mqioc_setattr(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_attr_req req;
	struct mq_handle_entry *he;
	long ret;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	he = mq_session_lookup(s, req.handle);
	if (!he)
		return -EBADF;

	ret = mq_do_setattr(he, &req.newattr, &req.attr);
	mq_handle_put(he);
	if (ret)
		return ret;

	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long mqueue_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mq_session *s = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case SHADOW_MQUEUE_IOC_ABI_VERSION: {
		u32 ver = SHADOW_MQUEUE_ABI_VERSION;

		return copy_to_user(uarg, &ver, sizeof(ver)) ? -EFAULT : 0;
	}
	case SHADOW_MQUEUE_IOC_OPEN:	  return mqioc_open(s, uarg);
	case SHADOW_MQUEUE_IOC_CLOSE:	  return mqioc_close(s, uarg);
	case SHADOW_MQUEUE_IOC_UNLINK:	  return mqioc_unlink(s, uarg);
	case SHADOW_MQUEUE_IOC_SEND:	  return mqioc_send(s, uarg);
	case SHADOW_MQUEUE_IOC_RECEIVE:	  return mqioc_receive(s, uarg);
	case SHADOW_MQUEUE_IOC_GETATTR:	  return mqioc_getattr(s, uarg);
	case SHADOW_MQUEUE_IOC_SETATTR:	  return mqioc_setattr(s, uarg);
	default:			  return -ENOTTY;
	}
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

static int mqueue_open(struct inode *inode, struct file *file)
{
	struct mq_session *s;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	xa_init(&s->handles);
	mutex_init(&s->lock);
	file->private_data = s;
	return 0;
}

static int mqueue_release(struct inode *inode, struct file *file)
{
	struct mq_session *s = file->private_data;
	struct mq_handle_entry *he;
	unsigned long id;

	if (!s)
		return 0;

	xa_for_each(&s->handles, id, he) {
		mq_handle_put(he);
	}
	xa_destroy(&s->handles);
	mutex_destroy(&s->lock);
	kfree(s);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations mqueue_fops = {
	.owner		= THIS_MODULE,
	.open		= mqueue_open,
	.release	= mqueue_release,
	.unlocked_ioctl	= mqueue_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice mqueue_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= SHADOW_MQUEUE_DEVICE_NAME,
	.fops	= &mqueue_fops,
	.mode	= 0600,
};

int __init shadow_mqueue_init(void)
{
	int ret;

	pr_info("shadow_mqueue: init: registering misc device %s\n",
		SHADOW_MQUEUE_DEVICE_PATH);
	ret = misc_register(&mqueue_miscdev);
	if (ret) {
		pr_err("shadow_mqueue: failed to register misc device: %d\n", ret);
		return ret;
	}
	pr_info("shadow_mqueue: init: misc device registered\n");

	pr_info("shadow_mqueue: init: installing transparent mq_* hooks\n");
	ret = shadow_hook_install_all(shadow_mqueue_hooks, "shadow_mqueue");
	if (ret < 0) {
		pr_err("shadow_mqueue: init: shadow_hook_install_all() failed: %d\n", ret);
		misc_deregister(&mqueue_miscdev);
		shadow_hook_remove_all(shadow_mqueue_hooks);
		return ret;
	}
	pr_info("shadow_mqueue: init: %d hook(s) installed\n", ret);

	pr_info("shadow_mqueue: simulated POSIX mqueue subsystem loaded (ABI v%d) at %s with transparent mq_* hooks\n",
		SHADOW_MQUEUE_ABI_VERSION, SHADOW_MQUEUE_DEVICE_PATH);
	return 0;
}

void shadow_mqueue_exit(void)
{
	struct shadow_mq *mq;
	struct hlist_node *tmp;
	int bkt;

	pr_info("shadow_mqueue: exit: removing transparent mq_* hooks\n");
	shadow_hook_remove_all(shadow_mqueue_hooks);
	pr_info("shadow_mqueue: exit: deregistering misc device\n");
	misc_deregister(&mqueue_miscdev);

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
