/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_mqueue - simulated POSIX message queue subsystem UAPI
 *
 * Stable ioctl ABI shared between the shadow_mqueue kernel module and any
 * userspace client that intentionally talks to /dev/shadow_mqueue.
 *
 * On kernels built without CONFIG_POSIX_MQUEUE the mq_open/mq_send/mq_receive
 * family of calls are unavailable.  runc uses POSIX message queues for
 * parent↔child synchronisation during container initialisation; this module
 * provides a fully functional in-kernel replacement.  Modern revisions hook
 * the real mq_* syscalls transparently for stock runtimes, while preserving
 * this ioctl ABI as a stable secondary interface.
 *
 * Unlike the namespace simulation in shadowns (bookkeeping only), message
 * queues here are *functional*: SEND and RECEIVE actually transfer bytes
 * through a kernel-side list, with priority ordering and blocking semantics
 * mirroring POSIX mq_send(3)/mq_receive(3).
 *
 * See ctr_patches/shadow_mqueue/README.md for design and limitations.
 */
#ifndef _UAPI_SHADOW_MQUEUE_H
#define _UAPI_SHADOW_MQUEUE_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SHADOW_MQUEUE_DEVICE_NAME	"shadow_mqueue"
#define SHADOW_MQUEUE_DEVICE_PATH	"/dev/shadow_mqueue"

/* Bump whenever the ioctl ABI below changes incompatibly. */
#define SHADOW_MQUEUE_ABI_VERSION	1

/* Maximum queue name length (without NUL terminator), matching POSIX. */
#define SHADOW_MQ_NAME_MAX	255
/* Maximum message payload size (bytes). */
#define SHADOW_MQ_MSGSIZE_MAX	256
/* Default and hard cap on the number of messages in a queue. */
#define SHADOW_MQ_MAXMSG_DEF	10
#define SHADOW_MQ_MAXMSG_MAX	1024

/*
 * Flags for shadow_mq_open_req.oflag.
 * Bit values are chosen to avoid conflicts with standard O_* flags while
 * remaining easy to check.
 */
#define SHADOW_MQ_O_CREAT	0x01	/* create queue if it does not exist */
#define SHADOW_MQ_O_EXCL	0x02	/* fail if queue already exists (with O_CREAT) */
#define SHADOW_MQ_O_NONBLOCK	0x04	/* non-blocking send/receive */
#define SHADOW_MQ_O_RDONLY	0x08	/* open for receive only  */
#define SHADOW_MQ_O_WRONLY	0x10	/* open for send only     */
#define SHADOW_MQ_O_RDWR	0x18	/* open for send+receive  */

/*
 * struct shadow_mq_attr - queue attributes, mirroring POSIX struct mq_attr.
 * @mq_flags:   SHADOW_MQ_O_NONBLOCK or 0
 * @mq_maxmsg:  maximum number of messages the queue can hold
 * @mq_msgsize: maximum message size in bytes
 * @mq_curmsgs: current number of messages (read-only, ignored on input)
 */
struct shadow_mq_attr {
	__s64 mq_flags;
	__s64 mq_maxmsg;
	__s64 mq_msgsize;
	__s64 mq_curmsgs;
};

/*
 * struct shadow_mq_open_req - open (and optionally create) a named queue.
 *
 * @name:   queue name; must begin with '/'; NUL-terminated within the array.
 * @oflag:  SHADOW_MQ_O_* flags
 * @mode:   permission bits used when creating (ignored on get)
 * @attr:   desired attributes (in, when creating; ignored on get);
 *          actual queue attributes are returned here on success.
 * @handle: per-session handle for subsequent SEND/RECEIVE/CLOSE calls (out)
 */
struct shadow_mq_open_req {
	char		      name[SHADOW_MQ_NAME_MAX + 1];
	__u32		      oflag;
	__u32		      mode;
	struct shadow_mq_attr attr;
	__u32		      handle;
	__u32		      _pad;
};

/* struct shadow_mq_close_req - close a per-session handle. */
struct shadow_mq_close_req {
	__u32 handle;
	__u32 _pad;
};

/* struct shadow_mq_unlink_req - remove a named queue by name. */
struct shadow_mq_unlink_req {
	char name[SHADOW_MQ_NAME_MAX + 1];
	__u8 _pad[3];
};

/*
 * struct shadow_mq_send_req - send a message to a queue.
 *
 * @handle:     per-session handle (in)
 * @prio:       message priority; higher value = higher priority (in)
 * @timeout_ns: nanosecond deadline relative to CLOCK_MONOTONIC.
 *              0            = return immediately (EAGAIN if queue full).
 *              UINT64_MAX   = block without timeout.
 *              other value  = wait at most this many nanoseconds.
 * @msg_len:    number of valid bytes in msg_data (in); must be <= mq_msgsize.
 * @msg_data:   message payload (in)
 */
struct shadow_mq_send_req {
	__u32 handle;
	__u32 prio;
	__u64 timeout_ns;
	__u32 msg_len;
	__u32 _pad;
	__u8  msg_data[SHADOW_MQ_MSGSIZE_MAX];
};

/*
 * struct shadow_mq_recv_req - receive a message from a queue.
 *
 * @handle:     per-session handle (in)
 * @prio:       priority of the received message (out)
 * @timeout_ns: same semantics as shadow_mq_send_req.timeout_ns (in)
 * @msg_len:    on entry: size of msg_data buffer (in, must be >= mq_msgsize);
 *              on return: actual number of bytes received (out).
 * @msg_data:   received message payload (out)
 */
struct shadow_mq_recv_req {
	__u32 handle;
	__u32 prio;
	__u64 timeout_ns;
	__u32 msg_len;
	__u32 _pad;
	__u8  msg_data[SHADOW_MQ_MSGSIZE_MAX];
};

/*
 * struct shadow_mq_attr_req - get or set queue attributes.
 *
 * GETATTR: @newattr is ignored; @attr receives the current attributes.
 * SETATTR: only @newattr.mq_flags (NONBLOCK bit) is honoured; @attr receives
 *          the previous attributes.
 */
struct shadow_mq_attr_req {
	__u32		      handle;
	__u32		      _pad;
	struct shadow_mq_attr newattr;
	struct shadow_mq_attr attr;
};

#define SHADOW_MQUEUE_IOC_MAGIC		'Q'

/* Return the ABI version implemented by the loaded module. */
#define SHADOW_MQUEUE_IOC_ABI_VERSION \
	_IOR(SHADOW_MQUEUE_IOC_MAGIC, 0, __u32)
/* Open (and optionally create) a named message queue. */
#define SHADOW_MQUEUE_IOC_OPEN \
	_IOWR(SHADOW_MQUEUE_IOC_MAGIC, 1, struct shadow_mq_open_req)
/* Close a per-session handle (does not destroy the queue). */
#define SHADOW_MQUEUE_IOC_CLOSE \
	_IOW(SHADOW_MQUEUE_IOC_MAGIC, 2, struct shadow_mq_close_req)
/* Remove a named queue; destroyed when all handles are closed. */
#define SHADOW_MQUEUE_IOC_UNLINK \
	_IOW(SHADOW_MQUEUE_IOC_MAGIC, 3, struct shadow_mq_unlink_req)
/* Send a message to a queue. */
#define SHADOW_MQUEUE_IOC_SEND \
	_IOW(SHADOW_MQUEUE_IOC_MAGIC, 4, struct shadow_mq_send_req)
/* Receive a message from a queue. */
#define SHADOW_MQUEUE_IOC_RECEIVE \
	_IOWR(SHADOW_MQUEUE_IOC_MAGIC, 5, struct shadow_mq_recv_req)
/* Get current queue attributes. */
#define SHADOW_MQUEUE_IOC_GETATTR \
	_IOWR(SHADOW_MQUEUE_IOC_MAGIC, 6, struct shadow_mq_attr_req)
/* Set queue attributes (only mq_flags is writable). */
#define SHADOW_MQUEUE_IOC_SETATTR \
	_IOWR(SHADOW_MQUEUE_IOC_MAGIC, 7, struct shadow_mq_attr_req)

#endif /* _UAPI_SHADOW_MQUEUE_H */
