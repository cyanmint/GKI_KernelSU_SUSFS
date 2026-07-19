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
 *
 * mount("mqueue", ...) fallback
 * -----------------------------
 * Besides the mq_* syscalls above, this module also hooks mount(2) itself.
 * Container runtimes (dockerd/containerd/runc) unconditionally attempt
 * `mount("mqueue", "/dev/mqueue", "mqueue", MS_NOSUID|MS_NODEV|MS_NOEXEC, ...)`
 * during container init. That call is expected to work whenever
 * CONFIG_POSIX_MQUEUE is real/builtin - but in practice it can still fail
 * with -ENODEV ("no such device") in the shadow_ns family's fake-namespace
 * environment (see shadow_ns_mnt/shadow_ns_pid/shadow_ns_user: pid/mnt/user
 * namespaces are refcounted bookkeeping only, not real, on kernels lacking
 * those Kconfig options), which confuses the real mqueue filesystem's
 * per-namespace tree lookup even though mqueue itself is genuinely
 * registered. hook_sys_mount() lets the real mount(2) run first and, only if
 * it fails with -ENODEV for fstype "mqueue", transparently retries the exact
 * same call with the filesystem type swapped for "tmpfs" (which accepts the
 * same handful of mount options runtimes pass here, e.g. "mode=", "size=",
 * and is always available since CONFIG_TMPFS/CONFIG_SHMEM are core VFS
 * features on every GKI kernel). This gives the runtime a real, working
 * mountpoint at /dev/mqueue without depending on the mqueue subsystem's
 * internal namespace plumbing at all. See mq_do_mount_fallback() below for
 * why this needs a scratch page instead of just poking the caller's memory.
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
static long (*real_sys_mount)(const struct pt_regs *regs);

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

#define SHADOW_MQ_MOUNT_FSTYPE_MAX 32
#define SHADOW_MQ_MOUNT_TYPE_ARG   2

/*
 * mq_do_mount_fallback() - retry a failed mount("mqueue", ...) as tmpfs.
 *
 * We cannot just call an in-kernel mount helper directly (do_mount()/
 * path_mount() are not exported, and the get_tree_nodev()/simple_fill_super()
 * pair used by an earlier revision to register a real "mqueue" pseudo-fs are
 * trimmed from production GKI kernels' exported-symbol table - see the
 * top-of-file comment). Instead we reuse the *real* mount(2) syscall
 * unmodified, just with its filesystem-type argument swapped out: vm_mmap()
 * (like mmap(2) itself) maps a throwaway anonymous page into the *calling
 * process's* address space - i.e. it returns an ordinary userspace address,
 * not a kernel one, so copy_to_user() below is the correct way to populate
 * it. We write "tmpfs" into that page, point a copy of the original pt_regs
 * at it instead of the caller's "mqueue" string, and call through to
 * @real_sys_mount with the copy. vm_mmap()/vm_munmap() are ordinary
 * EXPORT_SYMBOL() helpers used throughout the VFS/ELF loader, so - unlike
 * get_tree_nodev()/simple_fill_super() - they are never trimmed. The mapping
 * is a full page because do_mmap() internally requires (and rounds up to) a
 * page-aligned length regardless of what is requested; only the first few
 * bytes are ever written or read.
 *
 * The original dev_name/dir_name/flags/data arguments are passed through
 * unchanged: tmpfs accepts the same handful of options ("mode=", "size=",
 * "uid=", "gid=", ...) that runtimes typically pass for /dev/mqueue.
 */
static long mq_do_mount_fallback(const struct pt_regs *regs)
{
	struct pt_regs kregs;
	unsigned long scratch;
	long ret;

	memcpy(&kregs, regs, sizeof(kregs));

	scratch = vm_mmap(NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (IS_ERR_VALUE(scratch))
		return (long)scratch;

	/* sizeof("tmpfs") is 6 and deliberately includes the trailing NUL, so
	 * the string real_sys_mount() reads back out of userspace below is
	 * itself NUL-terminated. */
	if (copy_to_user((void __user *)scratch, "tmpfs", sizeof("tmpfs"))) {
		vm_munmap(scratch, PAGE_SIZE);
		return -EFAULT;
	}

	SHADOW_SYSCALL_SET_ARG(&kregs, SHADOW_MQ_MOUNT_TYPE_ARG, scratch);
	ret = real_sys_mount(&kregs);
	vm_munmap(scratch, PAGE_SIZE);
	return ret;
}

/*
 * hook_sys_mount() - let the real mount(2) run first; only retry as tmpfs
 * when it failed with -ENODEV for fstype "mqueue" specifically. Every other
 * fstype, and every other error (permissions, busy target, ...), passes
 * through untouched.
 */
static long hook_sys_mount(const struct pt_regs *regs)
{
	const char __user *utype =
		(const char __user *)SHADOW_SYSCALL_ARG(regs, SHADOW_MQ_MOUNT_TYPE_ARG);
	char type[SHADOW_MQ_MOUNT_FSTYPE_MAX];
	long copied;
	long ret;

	ret = real_sys_mount(regs);
	if (ret != -ENODEV || !utype)
		return ret;

	copied = strncpy_from_user(type, utype, sizeof(type));
	if (copied < 0 || copied >= sizeof(type))
		return ret;
	/*
	 * strncpy_from_user()'s @count includes room for the trailing NUL: on
	 * success (string shorter than @count) it returns the string length
	 * and has already written the NUL at type[copied]; if the source
	 * string didn't fit, it returns exactly sizeof(type) with no NUL
	 * written at all (see include/linux/uaccess.h / lib/strncpy_from_user.c
	 * docs: "If @count is smaller than the length of the string, copies
	 * @count bytes and returns @count"), which the ">= sizeof(type)" check
	 * above already rejects. The explicit NUL-termination here is
	 * therefore belt-and-braces, not a correctness fix, but keeps
	 * strcmp() provably safe regardless of kernel version quirks.
	 */
	type[copied] = '\0';
	if (strcmp(type, "mqueue"))
		return ret;

	pr_info_ratelimited(
		"shadow_mqueue: mount(\"mqueue\", ...) failed with -ENODEV; "
		"retrying as tmpfs so the caller sees a working mountpoint\n");
	return mq_do_mount_fallback(regs);
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

static const char * const mount_hook_names[] = {
	"__arm64_sys_mount",
	"sys_mount",
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
static struct shadow_hook mount_hook =
	SHADOW_HOOK(mount_hook_names, hook_sys_mount, &real_sys_mount);

static struct shadow_hook *shadow_mqueue_hooks[] = {
	&mq_open_hook,
	&mq_unlink_hook,
	&mq_timedsend_hook,
	&mq_timedreceive_hook,
	&mq_notify_hook,
	&mq_getsetattr_hook,
	&mount_hook,
	NULL,
};

/*
 * shadow_mqueue does not register a "mqueue" filesystem type of its own.
 *
 * An earlier revision did so (via register_filesystem()/get_tree_nodev()/
 * simple_fill_super()) purely so that `mount("mqueue", ..., "mqueue", ...)`
 * (which container runtimes such as runc unconditionally attempt during
 * container init) would succeed on a kernel built without
 * CONFIG_POSIX_MQUEUE. However, get_tree_nodev()/simple_fill_super() are
 * trimmed from the exported-symbol table of production GKI kernels (they are
 * not referenced by any built-in code, so CONFIG_TRIM_UNUSED_KSYMS drops
 * their EXPORT_SYMBOL entries even though the functions themselves remain in
 * the kernel image). Referencing them makes the whole module fail to load
 * with "Unknown symbol get_tree_nodev"/"Unknown symbol simple_fill_super"
 * (insmod surfaces this as ENOENT, i.e. "No such file or directory") on such
 * kernels, defeating the module's actual purpose.
 *
 * The mq_* syscalls hooked below are fully self-contained (they resolve
 * queues through the global name hash and anon-inode fds, not through any
 * on-disk/vfs state) and work regardless of whether a "mqueue" filesystem is
 * registered. The mount(2) hook above (hook_sys_mount()/
 * mq_do_mount_fallback()) covers the actual `mount("mqueue", ...)` call
 * itself instead: it lets the real syscall run first and only substitutes a
 * "tmpfs" mount when that fails with -ENODEV, without ever needing a
 * trimmed-symbol pseudo-filesystem registration.
 */

/*
 * /dev/mqueue proactive creation
 * -------------------------------
 * The mount(2) hook above only helps when *some* userspace process actually
 * calls mount("mqueue", "/dev/mqueue", "mqueue", ...) *after* this module is
 * loaded. In practice shadow_mqueue is typically insmod'd late (e.g. as a
 * KernelSU/Magisk post-fs-data module), well after init.rc's own one-shot
 * `mount mqueue mqueue /dev/mqueue ...` line already ran (and silently failed
 * with -ENODEV, since init never retries a failed boot-time mount). That
 * leaves /dev/mqueue nonexistent or an empty, never-mounted directory for
 * the remainder of boot, and the reactive hook never gets a chance to fire.
 *
 * mq_dev_mqueue_ensure() fixes this by proactively creating the mountpoint
 * (mkdir -p equivalent) and mounting tmpfs on it directly from module init,
 * the same way the hook's fallback would have, without waiting for a mount(2)
 * call that may never come. The reactive hook_sys_mount() above is left in
 * place regardless, both as a fallback for kernels/paths this best-effort
 * helper cannot handle and to keep serving any later mount("mqueue", ...)
 * attempt (e.g. from a container's own /dev setup) that targets some other
 * mount namespace.
 *
 * None of path_mount()/vfs_mkdir()/kern_path_create()/done_path_create() are
 * referenced directly by name: path_mount() is not EXPORT_SYMBOL()'d at all
 * on any of our target KMIs (it is fs/namespace.c-internal), and vfs_mkdir()
 * is EXPORT_SYMBOL_NS()'d under "ANDROID_GKI_VFS_EXPORT_ONLY" on several GKI
 * branches - a module that references it directly would need to
 * MODULE_IMPORT_NS() that namespace, which is deliberately reserved for a
 * small allow-list of in-tree consumers (fuse, overlayfs, ...) and may be
 * refused outright on production kernels. Resolving all four via
 * shadow_hook_resolve() (the same register_kprobe()-based address lookup
 * used for the mq_* syscalls and the mount(2) hook above) sidesteps both
 * problems: kprobe address resolution walks kallsyms directly and does not
 * care whether a symbol is EXPORT_SYMBOL()'d, namespaced, or trimmed.
 */

#define SHADOW_MQ_DEV_MQUEUE_PATH "/dev/mqueue"
#define SHADOW_MQ_DEV_MQUEUE_MODE 0755

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
typedef int (*mq_vfs_mkdir_fn)(struct mnt_idmap *, struct inode *,
				struct dentry *, umode_t);
#define MQ_VFS_MKDIR(fn, dir, dentry, mode) \
	(fn)(&nop_mnt_idmap, (dir), (dentry), (mode))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
typedef int (*mq_vfs_mkdir_fn)(struct user_namespace *, struct inode *,
				struct dentry *, umode_t);
#define MQ_VFS_MKDIR(fn, dir, dentry, mode) \
	(fn)(&init_user_ns, (dir), (dentry), (mode))
#else
typedef int (*mq_vfs_mkdir_fn)(struct inode *, struct dentry *, umode_t);
#define MQ_VFS_MKDIR(fn, dir, dentry, mode) \
	(fn)((dir), (dentry), (mode))
#endif

typedef int (*mq_kern_path_fn)(const char *, unsigned int, struct path *);
typedef struct dentry *(*mq_kern_path_create_fn)(int, const char *,
						  struct path *, unsigned int);
typedef void (*mq_done_path_create_fn)(struct path *, struct dentry *);
typedef int (*mq_path_mount_fn)(const char *, struct path *, const char *,
				 unsigned long, void *);

/*
 * mq_dev_mqueue_do_mount() - mount tmpfs at an already-resolved @path.
 * @path is left untouched (caller still owns/puts the reference); returns
 * the underlying path_mount() result.
 */
static int mq_dev_mqueue_do_mount(mq_path_mount_fn path_mount_fn,
				   struct path *path)
{
	return path_mount_fn("mqueue", path, "tmpfs",
			      MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
}

static void mq_dev_mqueue_ensure(void)
{
	mq_kern_path_fn kern_path_fn;
	mq_kern_path_create_fn kern_path_create_fn;
	mq_done_path_create_fn done_path_create_fn;
	mq_vfs_mkdir_fn vfs_mkdir_fn;
	mq_path_mount_fn path_mount_fn;
	struct path path;
	struct path create_path;
	struct dentry *dentry;
	int ret;

	kern_path_fn = (mq_kern_path_fn)shadow_hook_resolve("kern_path");
	kern_path_create_fn = (mq_kern_path_create_fn)
		shadow_hook_resolve("kern_path_create");
	done_path_create_fn = (mq_done_path_create_fn)
		shadow_hook_resolve("done_path_create");
	vfs_mkdir_fn = (mq_vfs_mkdir_fn)shadow_hook_resolve("vfs_mkdir");
	path_mount_fn = (mq_path_mount_fn)shadow_hook_resolve("path_mount");

	if (!kern_path_fn || !kern_path_create_fn || !done_path_create_fn ||
	    !vfs_mkdir_fn || !path_mount_fn) {
		pr_info("shadow_mqueue: init: could not resolve VFS helpers for "
			 "proactive " SHADOW_MQ_DEV_MQUEUE_PATH " mount; falling "
			 "back to the reactive mount(2) hook only\n");
		return;
	}

	ret = kern_path_fn(SHADOW_MQ_DEV_MQUEUE_PATH, LOOKUP_DIRECTORY, &path);
	if (!ret) {
		/* Something is already mounted at this path (real mqueue, a
		 * previous tmpfs fallback, ...): leave it alone. */
		if (path.dentry == path.dentry->d_sb->s_root) {
			pr_info("shadow_mqueue: init: " SHADOW_MQ_DEV_MQUEUE_PATH
				" is already a mountpoint; leaving it as-is\n");
			path_put(&path);
			return;
		}

		ret = mq_dev_mqueue_do_mount(path_mount_fn, &path);
		path_put(&path);
		if (ret)
			pr_info("shadow_mqueue: init: proactive tmpfs mount on "
				 "existing " SHADOW_MQ_DEV_MQUEUE_PATH
				 " failed: %d\n", ret);
		else
			pr_info("shadow_mqueue: init: mounted tmpfs on existing "
				 SHADOW_MQ_DEV_MQUEUE_PATH "\n");
		return;
	}

	if (ret != -ENOENT) {
		pr_info("shadow_mqueue: init: kern_path(%s) failed: %d\n",
			 SHADOW_MQ_DEV_MQUEUE_PATH, ret);
		return;
	}

	/* /dev/mqueue does not exist yet: create it, then mount tmpfs on it. */
	dentry = kern_path_create_fn(AT_FDCWD, SHADOW_MQ_DEV_MQUEUE_PATH,
				      &create_path, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry)) {
		pr_info("shadow_mqueue: init: kern_path_create(%s) failed: %ld\n",
			 SHADOW_MQ_DEV_MQUEUE_PATH, PTR_ERR(dentry));
		return;
	}

	ret = MQ_VFS_MKDIR(vfs_mkdir_fn, d_inode(create_path.dentry), dentry,
			    SHADOW_MQ_DEV_MQUEUE_MODE);
	done_path_create_fn(&create_path, dentry);
	if (ret) {
		pr_info("shadow_mqueue: init: mkdir(%s) failed: %d\n",
			 SHADOW_MQ_DEV_MQUEUE_PATH, ret);
		return;
	}

	ret = kern_path_fn(SHADOW_MQ_DEV_MQUEUE_PATH, LOOKUP_DIRECTORY, &path);
	if (ret) {
		pr_info("shadow_mqueue: init: kern_path(%s) failed after mkdir: %d\n",
			 SHADOW_MQ_DEV_MQUEUE_PATH, ret);
		return;
	}

	ret = mq_dev_mqueue_do_mount(path_mount_fn, &path);
	path_put(&path);
	if (ret)
		pr_info("shadow_mqueue: init: created " SHADOW_MQ_DEV_MQUEUE_PATH
			 " but tmpfs mount failed: %d\n", ret);
	else
		pr_info("shadow_mqueue: init: created and mounted "
			 SHADOW_MQ_DEV_MQUEUE_PATH "\n");
}

int __init shadow_mqueue_init(void)
{
	int ret;

	pr_info("shadow_mqueue: init: installing transparent mq_*/mount hooks\n");
	ret = shadow_hook_install_all(shadow_mqueue_hooks, "shadow_mqueue");
	if (ret < 0) {
		pr_err("shadow_mqueue: init: shadow_hook_install_all() failed: %d\n", ret);
		shadow_hook_remove_all(shadow_mqueue_hooks);
		return ret;
	}
	pr_info("shadow_mqueue: init: %d hook(s) installed\n", ret);

	mq_dev_mqueue_ensure();

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
MODULE_DESCRIPTION("Simulated POSIX message queue subsystem with transparent mq_*/mount syscall hijacking");
MODULE_VERSION(SHADOW_MQUEUE_VERSION);
