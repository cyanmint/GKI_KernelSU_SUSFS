// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_sysvipc message-queue payload transfer (msgsnd/msgrcv).
 *
 * shadow_sysvipc_registry.c only tracks resource identity/bookkeeping; this
 * file adds the actual in-kernel message payload storage for TYPE_MSGQ
 * resources so msgsnd(2)/msgrcv(2) behave like the real SysV IPC message
 * queue instead of merely falling through to -ENOSYS.
 *
 * Semantics mirror ipc/msg.c closely enough for real-world callers:
 *   - msgsnd() copies mtype + payload from userspace and appends to the
 *     queue's message list (FIFO), blocking (unless IPC_NOWAIT) when the
 *     queue is at its byte/message-count capacity.
 *   - msgrcv() with msgtyp == 0 takes the oldest message; msgtyp > 0 takes
 *     the oldest message with an exact matching mtype; msgtyp < 0 takes the
 *     oldest message among those with the lowest mtype <= |msgtyp|.  It
 *     blocks (unless IPC_NOWAIT) until a matching message is queued.
 */
#include "shadow_sysvipc_internal.h"

/* Free every queued message. Caller must hold no other reference to @res
 * (invoked only from svipc_put()/svipc_force_free_all_resources() once the
 * resource has already been unpublished and its refcount has dropped to 0).
 */
void svipc_msgq_purge_locked(struct svipc_resource *res)
{
	struct svipc_msg *msg, *tmp;

	list_for_each_entry_safe(msg, tmp, &res->msgs, node) {
		list_del(&msg->node);
		kfree(msg);
	}
	res->msg_count = 0;
	res->msg_qbytes = 0;
}

long svipc_sys_msgsnd(int msqid, const void __user *umsgp, size_t msgsz,
		      int msgflg)
{
	struct svipc_resource *res;
	struct svipc_msg *msg;
	long mtype;
	int ret;

	if (msgsz > SHADOW_SYSVIPC_MSG_MAXSIZE)
		return -EINVAL;

	res = svipc_get(msqid);
	if (!res)
		return -EINVAL;
	if (res->type != SHADOW_SYSVIPC_TYPE_MSGQ) {
		ret = -EINVAL;
		goto out_put;
	}

	if (get_user(mtype, (const long __user *)umsgp)) {
		ret = -EFAULT;
		goto out_put;
	}
	if (mtype < 1) {
		ret = -EINVAL;
		goto out_put;
	}

	msg = kmalloc(sizeof(*msg) + msgsz, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out_put;
	}
	msg->mtype = mtype;
	msg->len = msgsz;
	if (msgsz && copy_from_user(msg->data,
				    (const char __user *)umsgp + sizeof(long),
				    msgsz)) {
		kfree(msg);
		ret = -EFAULT;
		goto out_put;
	}

	for (;;) {
		mutex_lock(&res->msgs_lock);
		if (res->msg_count < SHADOW_SYSVIPC_MSG_MAXCOUNT &&
		    res->msg_qbytes + msgsz <= SHADOW_SYSVIPC_MSG_MAXQBYTES) {
			list_add_tail(&msg->node, &res->msgs);
			res->msg_count++;
			res->msg_qbytes += msgsz;
			mutex_unlock(&res->msgs_lock);
			wake_up_interruptible_all(&res->msgs_wait);
			ret = 0;
			goto out_put;
		}
		mutex_unlock(&res->msgs_lock);

		if (msgflg & IPC_NOWAIT) {
			kfree(msg);
			ret = -EAGAIN;
			goto out_put;
		}

		ret = wait_event_interruptible(res->msgs_wait,
			res->msg_count < SHADOW_SYSVIPC_MSG_MAXCOUNT &&
			res->msg_qbytes + msgsz <= SHADOW_SYSVIPC_MSG_MAXQBYTES);
		if (ret) {
			kfree(msg);
			ret = -ERESTARTSYS;
			goto out_put;
		}
	}

out_put:
	svipc_put(res);
	return ret;
}

/* Find (without removing) the message matching @msgtyp per SysV msgrcv()
 * semantics. Caller must hold @res->msgs_lock.
 */
static struct svipc_msg *svipc_msgq_find_locked(struct svipc_resource *res,
						long msgtyp)
{
	struct svipc_msg *msg, *best = NULL;

	if (msgtyp == 0)
		return list_first_entry_or_null(&res->msgs, struct svipc_msg,
						 node);

	if (msgtyp > 0) {
		list_for_each_entry(msg, &res->msgs, node) {
			if (msg->mtype == msgtyp)
				return msg;
		}
		return NULL;
	}

	/* msgtyp < 0: lowest mtype <= |msgtyp|, oldest among ties. */
	list_for_each_entry(msg, &res->msgs, node) {
		if (msg->mtype <= -msgtyp &&
		    (!best || msg->mtype < best->mtype))
			best = msg;
	}
	return best;
}

long svipc_sys_msgrcv(int msqid, void __user *umsgp, size_t msgsz,
		      long msgtyp, int msgflg)
{
	struct svipc_resource *res;
	struct svipc_msg *msg;
	size_t copylen;
	long ret;

	res = svipc_get(msqid);
	if (!res)
		return -EINVAL;
	if (res->type != SHADOW_SYSVIPC_TYPE_MSGQ) {
		ret = -EINVAL;
		goto out_put;
	}

	for (;;) {
		mutex_lock(&res->msgs_lock);
		msg = svipc_msgq_find_locked(res, msgtyp);
		if (msg) {
			copylen = msg->len;
			if (copylen > msgsz) {
				if (!(msgflg & MSG_NOERROR)) {
					mutex_unlock(&res->msgs_lock);
					ret = -E2BIG;
					goto out_put;
				}
				copylen = msgsz;
			}
			list_del(&msg->node);
			res->msg_count--;
			res->msg_qbytes -= msg->len;
			mutex_unlock(&res->msgs_lock);
			wake_up_interruptible_all(&res->msgs_wait);

			if (put_user(msg->mtype, (long __user *)umsgp)) {
				kfree(msg);
				ret = -EFAULT;
				goto out_put;
			}
			if (copylen && copy_to_user((char __user *)umsgp + sizeof(long),
						    msg->data, copylen)) {
				kfree(msg);
				ret = -EFAULT;
				goto out_put;
			}
			kfree(msg);
			ret = copylen;
			goto out_put;
		}
		mutex_unlock(&res->msgs_lock);

		if (msgflg & IPC_NOWAIT) {
			ret = -ENOMSG;
			goto out_put;
		}

		/*
		 * The wait condition intentionally only checks msg_count (a
		 * plain, lock-free read) rather than walking the message list
		 * to find a msgtyp match: list traversal without msgs_lock
		 * held would race with concurrent msgsnd()/msgrcv() list
		 * mutation. This may produce a spurious wakeup when messages
		 * are queued but none match @msgtyp, which is harmless: the
		 * top of the loop re-locks and calls svipc_msgq_find_locked()
		 * again, and simply loops back into wait_event_interruptible()
		 * if there is still no match.
		 */
		ret = wait_event_interruptible(res->msgs_wait,
						READ_ONCE(res->msg_count) != 0);
		if (ret) {
			ret = -ERESTARTSYS;
			goto out_put;
		}
	}

out_put:
	svipc_put(res);
	return ret;
}
