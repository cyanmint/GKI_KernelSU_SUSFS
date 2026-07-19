// SPDX-License-Identifier: GPL-2.0
#include "shadow_mqueue_internal.h"

static void mq_enqueue_locked(struct shadow_mq *mq, struct mq_msg *m);

long mq_do_send(struct mq_handle_entry *he, const void *msg_data, u32 msg_len,
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

long mq_do_receive(struct mq_handle_entry *he, void *msg_data, u32 *msg_len,
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

struct mq_handle_entry *mq_get_shadow_handle_from_fd(int mqdes)
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


int mq_create_anon_fd(const char *name, struct mq_handle_entry *he, int oflag,
		      bool created)
{
	int fd;

	fd = anon_inode_getfd("shadow_mqueue", &shadow_mq_anon_fops, he,
			      O_RDWR | (oflag & O_CLOEXEC));
	if (fd < 0) {
		if (created)
			mq_do_unlink(name);
		mq_handle_put(he);
	}
	return fd;
}
