// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_mqueue - simulated POSIX message queue subsystem
 *
 * A standalone loadable kernel module that provides fully functional POSIX
 * message queues through ioctls on the /dev/shadow_mqueue misc device.
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
 * shadow_mqueue provides a drop-in replacement: a patched runc opens
 * /dev/shadow_mqueue, creates a named virtual queue, and drives SEND/RECEIVE
 * operations through ioctls exactly as it would have used mq_open/mq_send/
 * mq_receive.  Message transfer is fully functional: bytes written by a sender
 * are buffered in a kernel-side priority-ordered list and delivered to the
 * receiver, with blocking and timeout semantics mirroring the POSIX standard.
 *
 * Architecture
 * ------------
 * - Named queues persist in a global hash table keyed by name until unlinked.
 * - Each open("/dev/shadow_mqueue") creates a *session*; sessions allocate
 *   per-session *handles* for individual queue opens.
 * - Messages are stored as heap-allocated mq_msg objects in a priority list.
 * - Receivers block on a wait_queue_head_t; senders wake them on each enqueue.
 *   Senders similarly block when a queue is full and are woken on dequeue.
 * - All resources (handles → queues → messages) are freed on fd close, so a
 *   crashed runtime never leaks kernel memory.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
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
};

/* Per-open-fd session. */
struct mq_session {
	struct xarray	handles; /* handle_id -> mq_handle_entry * */
	struct mutex	lock;
};

/* Global name registry. */
static DEFINE_HASHTABLE(mq_name_hash, SHADOW_MQ_NAME_HTBITS);
static DEFINE_MUTEX(mq_name_lock);

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
	struct shadow_mq *mq = NULL;
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
	ret = -EINVAL;
	if (!req->name[0] || req->name[0] != '/')
		goto out_req;

	/* Clamp mq_maxmsg and mq_msgsize. */
	if (req->attr.mq_maxmsg <= 0 ||
	    req->attr.mq_maxmsg > SHADOW_MQ_MAXMSG_MAX)
		req->attr.mq_maxmsg = SHADOW_MQ_MAXMSG_DEF;
	if (req->attr.mq_msgsize <= 0 ||
	    req->attr.mq_msgsize > SHADOW_MQ_MSGSIZE_MAX)
		req->attr.mq_msgsize = SHADOW_MQ_MSGSIZE_MAX;

	/*
	 * Find-or-create under the name lock so two concurrent creates with
	 * the same name do not both allocate new queue objects.
	 */
	mutex_lock(&mq_name_lock);
	mq = mq_find_locked(req->name);
	if (mq) {
		if ((req->oflag & SHADOW_MQ_O_CREAT) &&
		    (req->oflag & SHADOW_MQ_O_EXCL)) {
			mutex_unlock(&mq_name_lock);
			ret = -EEXIST;
			goto out_req;
		}
		if (READ_ONCE(mq->unlinked)) {
			mutex_unlock(&mq_name_lock);
			ret = -ENOENT;
			goto out_req;
		}
		mq_get(mq);
	} else {
		if (!(req->oflag & SHADOW_MQ_O_CREAT)) {
			mutex_unlock(&mq_name_lock);
			ret = -ENOENT;
			goto out_req;
		}
		mq = kzalloc(sizeof(*mq), GFP_KERNEL);
		if (!mq) {
			mutex_unlock(&mq_name_lock);
			ret = -ENOMEM;
			goto out_req;
		}
		strscpy(mq->name, req->name, sizeof(mq->name));
		mq->mq_maxmsg  = req->attr.mq_maxmsg;
		mq->mq_msgsize = req->attr.mq_msgsize;
		mq->nonblock   = !!(req->oflag & SHADOW_MQ_O_NONBLOCK);
		atomic_set(&mq->curmsgs, 0);
		spin_lock_init(&mq->msgs_lock);
		INIT_LIST_HEAD(&mq->msgs);
		init_waitqueue_head(&mq->wait_recv);
		init_waitqueue_head(&mq->wait_send);
		/*
		 * Start with refcount=2: one reference for the name-hash entry
		 * and one for the first handle.  This mirrors the mq_get() call
		 * made for an already-existing queue found above, so that the
		 * he->mq reference and the name-hash reference are always
		 * tracked separately and a handle close never frees a queue that
		 * is still in the hash.
		 */
		refcount_set(&mq->refcount, 2); /* name-hash ref + first handle ref */
		hash_add(mq_name_hash, &mq->name_node,
			 mq_name_hash_val(req->name));
		created = true;
	}
	mutex_unlock(&mq_name_lock);

	he = kzalloc(sizeof(*he), GFP_KERNEL);
	if (!he) {
		mq_put(mq); /* drop handle ref; name-hash ref remains for created queue */
		ret = -ENOMEM;
		goto out_req;
	}
	he->mq    = mq;
	he->oflag = req->oflag;

	mutex_lock(&s->lock);
	ret = xa_alloc(&s->handles, &handle, he, XA_LIMIT(1, INT_MAX),
		       GFP_KERNEL);
	mutex_unlock(&s->lock);
	if (ret) {
		kfree(he);
		mq_put(mq);
		goto out_req;
	}

	/* Return actual queue attributes. */
	req->attr.mq_flags   = READ_ONCE(mq->nonblock) ? SHADOW_MQ_O_NONBLOCK : 0;
	req->attr.mq_maxmsg  = mq->mq_maxmsg;
	req->attr.mq_msgsize = mq->mq_msgsize;
	req->attr.mq_curmsgs = (s64)atomic_read(&mq->curmsgs);
	req->handle = handle;

	ret = copy_to_user(arg, req, sizeof(*req)) ? -EFAULT : 0;
	if (ret) {
		/* Failed to return the handle to userspace; undo allocation. */
		mutex_lock(&s->lock);
		xa_erase(&s->handles, handle);
		mutex_unlock(&s->lock);
		kfree(he);
		mq_put(mq); /* drop handle ref */
		if (created) {
			/*
			 * Also remove the newly created queue from the name hash
			 * and drop the name-hash reference so the queue is freed.
			 * Without this, a queue with no reachable handle would
			 * leak permanently in the name hash.
			 */
			mutex_lock(&mq_name_lock);
			hash_del(&mq->name_node);
			WRITE_ONCE(mq->unlinked, true);
			mutex_unlock(&mq_name_lock);
			mq_put(mq); /* drop name-hash ref */
		}
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

	mq_put(he->mq);
	kfree(he);
	return 0;
}

static long mqioc_unlink(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_unlink_req req;
	struct shadow_mq *mq;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	req.name[SHADOW_MQ_NAME_MAX] = '\0';

	mutex_lock(&mq_name_lock);
	mq = mq_find_locked(req.name);
	if (!mq) {
		mutex_unlock(&mq_name_lock);
		return -ENOENT;
	}
	hash_del(&mq->name_node);
	WRITE_ONCE(mq->unlinked, true);
	mutex_unlock(&mq_name_lock);

	/* Wake any blocked waiters so they can observe unlinked and return. */
	wake_up_all(&mq->wait_send);
	wake_up_all(&mq->wait_recv);

	mq_put(mq); /* drop the name-hash reference */
	return 0;
}

static long mqioc_send(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_send_req *req;
	struct mq_handle_entry *he;
	struct shadow_mq *mq;
	struct mq_msg *msg;
	bool nonblocking;
	long ret = 0;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(req, arg, sizeof(*req)))
		goto out_req;

	ret = -EBADF;
	mutex_lock(&s->lock);
	he = xa_load(&s->handles, req->handle);
	if (he)
		mq_get(he->mq);
	mutex_unlock(&s->lock);
	if (!he)
		goto out_req;

	mq = he->mq;
	nonblocking = !!(he->oflag & SHADOW_MQ_O_NONBLOCK) ||
		      req->timeout_ns == 0;

	ret = -EINVAL;
	if (req->msg_len == 0 || (s64)req->msg_len > mq->mq_msgsize)
		goto out_mq;

	msg = kmalloc(sizeof(*msg) + req->msg_len, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out_mq;
	}
	msg->prio = req->prio;
	msg->len  = req->msg_len;
	memcpy(msg->data, req->msg_data, req->msg_len);

	/* Wait for space in the queue. */
	for (;;) {
		spin_lock(&mq->msgs_lock);
		if (atomic_read(&mq->curmsgs) < (int)mq->mq_maxmsg) {
			mq_enqueue_locked(mq, msg);
			spin_unlock(&mq->msgs_lock);
			wake_up(&mq->wait_recv);
			ret = 0;
			goto out_mq;
		}
		spin_unlock(&mq->msgs_lock);

		if (READ_ONCE(mq->unlinked)) {
			ret = -ENOENT;
			break;
		}
		if (nonblocking) {
			ret = -EAGAIN;
			break;
		}

		if (req->timeout_ns == U64_MAX) {
			ret = wait_event_interruptible(mq->wait_send,
				atomic_read(&mq->curmsgs) < (int)mq->mq_maxmsg ||
				READ_ONCE(mq->unlinked));
		} else {
			long j = nsecs_to_jiffies(req->timeout_ns) + 1;

			ret = wait_event_interruptible_timeout(mq->wait_send,
				atomic_read(&mq->curmsgs) < (int)mq->mq_maxmsg ||
				READ_ONCE(mq->unlinked), j);
			if (ret == 0) {
				ret = -ETIMEDOUT;
				break;
			}
		}
		if (ret < 0) {
			ret = -EINTR;
			break;
		}
		ret = 0;
	}

	kfree(msg);
out_mq:
	mq_put(mq);
out_req:
	kfree(req);
	return ret;
}

static long mqioc_receive(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_recv_req *req;
	struct mq_handle_entry *he;
	struct shadow_mq *mq;
	struct mq_msg *msg = NULL;
	bool nonblocking;
	long ret = 0;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	ret = -EFAULT;
	if (copy_from_user(req, arg, sizeof(*req)))
		goto out_req;

	ret = -EBADF;
	mutex_lock(&s->lock);
	he = xa_load(&s->handles, req->handle);
	if (he)
		mq_get(he->mq);
	mutex_unlock(&s->lock);
	if (!he)
		goto out_req;

	mq = he->mq;
	nonblocking = !!(he->oflag & SHADOW_MQ_O_NONBLOCK) ||
		      req->timeout_ns == 0;

	/* Wait for a message. */
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

		if (READ_ONCE(mq->unlinked)) {
			ret = -ENOENT;
			goto out_mq;
		}
		if (nonblocking) {
			ret = -EAGAIN;
			goto out_mq;
		}

		if (req->timeout_ns == U64_MAX) {
			ret = wait_event_interruptible(mq->wait_recv,
				atomic_read(&mq->curmsgs) > 0 ||
				READ_ONCE(mq->unlinked));
		} else {
			long j = nsecs_to_jiffies(req->timeout_ns) + 1;

			ret = wait_event_interruptible_timeout(mq->wait_recv,
				atomic_read(&mq->curmsgs) > 0 ||
				READ_ONCE(mq->unlinked), j);
			if (ret == 0) {
				ret = -ETIMEDOUT;
				goto out_mq;
			}
		}
		if (ret < 0) {
			ret = -EINTR;
			goto out_mq;
		}
		ret = 0;
	}

	/* msg was dequeued; deliver it. */
	if (msg->len > req->msg_len) {
		kfree(msg);
		ret = -EMSGSIZE;
		goto out_mq;
	}
	req->prio    = msg->prio;
	req->msg_len = msg->len;
	memcpy(req->msg_data, msg->data, msg->len);
	kfree(msg);

	if (copy_to_user(arg, req, sizeof(*req)))
		ret = -EFAULT;
out_mq:
	mq_put(mq);
out_req:
	kfree(req);
	return ret;
}

static long mqioc_getattr(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_attr_req req;
	struct mq_handle_entry *he;
	struct shadow_mq *mq;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&s->lock);
	he = xa_load(&s->handles, req.handle);
	if (he)
		mq_get(he->mq);
	mutex_unlock(&s->lock);
	if (!he)
		return -EBADF;

	mq = he->mq;
	req.attr.mq_flags   = READ_ONCE(mq->nonblock) ? SHADOW_MQ_O_NONBLOCK : 0;
	req.attr.mq_maxmsg  = mq->mq_maxmsg;
	req.attr.mq_msgsize = mq->mq_msgsize;
	req.attr.mq_curmsgs = (s64)atomic_read(&mq->curmsgs);
	mq_put(mq);

	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long mqioc_setattr(struct mq_session *s, void __user *arg)
{
	struct shadow_mq_attr_req req;
	struct mq_handle_entry *he;
	struct shadow_mq *mq;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&s->lock);
	he = xa_load(&s->handles, req.handle);
	if (he)
		mq_get(he->mq);
	mutex_unlock(&s->lock);
	if (!he)
		return -EBADF;

	mq = he->mq;
	/* Return old attributes. */
	req.attr.mq_flags   = READ_ONCE(mq->nonblock) ? SHADOW_MQ_O_NONBLOCK : 0;
	req.attr.mq_maxmsg  = mq->mq_maxmsg;
	req.attr.mq_msgsize = mq->mq_msgsize;
	req.attr.mq_curmsgs = (s64)atomic_read(&mq->curmsgs);

	/* Apply new NONBLOCK flag (the only writable attribute). */
	WRITE_ONCE(mq->nonblock,
		   !!(req.newattr.mq_flags & SHADOW_MQ_O_NONBLOCK));
	if (READ_ONCE(mq->nonblock))
		he->oflag |= SHADOW_MQ_O_NONBLOCK;
	else
		he->oflag &= ~SHADOW_MQ_O_NONBLOCK;
	mq_put(mq);

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
		mq_put(he->mq);
		kfree(he);
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
	.llseek		= no_llseek,
};

static struct miscdevice mqueue_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= SHADOW_MQUEUE_DEVICE_NAME,
	.fops	= &mqueue_fops,
	.mode	= 0600,
};

static int __init shadow_mqueue_init(void)
{
	int ret;

	ret = misc_register(&mqueue_miscdev);
	if (ret) {
		pr_err("shadow_mqueue: failed to register misc device: %d\n", ret);
		return ret;
	}
	pr_info("shadow_mqueue: simulated POSIX mqueue subsystem loaded (ABI v%d) at %s\n",
		SHADOW_MQUEUE_ABI_VERSION, SHADOW_MQUEUE_DEVICE_PATH);
	return 0;
}

static void __exit shadow_mqueue_exit(void)
{
	struct shadow_mq *mq;
	struct hlist_node *tmp;
	int bkt;

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

module_init(shadow_mqueue_init);
module_exit(shadow_mqueue_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Simulated POSIX message queue subsystem for patched containerd");
MODULE_VERSION("1.0");
