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
 * environment (see shadow_ns/README.md: pid/mnt/user namespaces get real or
 * bookkeeping-only simulation depending on whether this kernel build's
 * CONFIG_PID_NS/USER_NS are absent; mount namespaces (CLONE_NEWNS) are always
 * builtin), which can confuse the real mqueue filesystem's per-namespace tree
 * lookup even though mqueue itself is genuinely registered. hook_sys_mount() lets the real mount(2) run first and, only if
 * it fails with -ENODEV for fstype "mqueue", transparently retries the exact
 * same call with the filesystem type swapped for "tmpfs" (which accepts the
 * same handful of mount options runtimes pass here, e.g. "mode=", "size=",
 * and is always available since CONFIG_TMPFS/CONFIG_SHMEM are core VFS
 * features on every GKI kernel). This gives the runtime a real, working
 * mountpoint at /dev/mqueue without depending on the mqueue subsystem's
 * internal namespace plumbing at all. See mq_do_mount_fallback() below for
 * why this needs a scratch page instead of just poking the caller's memory.
 */

#include "shadow_mqueue_internal.h"

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

void mq_put(struct shadow_mq *mq)
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

void mq_handle_put(struct mq_handle_entry *he)
{
	if (!he)
		return;
	if (!refcount_dec_and_test(&he->refcount))
		return;
	mq_put(he->mq);
	kfree(he);
}

bool mq_handle_get(struct mq_handle_entry *he)
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

void mq_fill_posix_attr_from_shadow(const struct shadow_mq_attr *shadow,
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

u32 mq_posix_to_shadow_oflag(int oflag)
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

int mq_copy_name_from_user(const char __user *uname, char *name)
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

int mq_wait_spec_from_abs_timeout(struct mq_wait_spec *wait, u32 oflag,
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

long mq_do_unlink(const char *name)
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

long mq_do_open(const char *name, u32 oflag,
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

long mq_do_getattr(struct mq_handle_entry *he, struct shadow_mq_attr *attr)
{
	if (!he || !attr)
		return -EINVAL;

	mq_fill_shadow_attr(he->mq, attr);
	return 0;
}

long mq_do_setattr(struct mq_handle_entry *he,
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


void mq_release_all(void)
{
	struct shadow_mq *mq;
	struct hlist_node *tmp;
	int bkt;

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
		mq_wake_waiters(mq);
		mq_put(mq); /* name-hash reference */
	}
	mutex_unlock(&mq_name_lock);
}
