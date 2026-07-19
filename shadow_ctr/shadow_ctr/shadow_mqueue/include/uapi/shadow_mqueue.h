/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_mqueue - simulated POSIX message queue subsystem UAPI
 *
 * Shared shadow mqueue constants and internal attribute layout used by the
 * transparent syscall-hook implementation. The shadow_mqueue module now
 * exposes no userspace ioctl ABI.
 */
#ifndef _UAPI_SHADOW_MQUEUE_H
#define _UAPI_SHADOW_MQUEUE_H

#include <linux/types.h>

/* Maximum queue name length (without NUL terminator), matching POSIX. */
#define SHADOW_MQ_NAME_MAX	255
/* Maximum message payload size (bytes). */
#define SHADOW_MQ_MSGSIZE_MAX	256
/* Default and hard cap on the number of messages in a queue. */
#define SHADOW_MQ_MAXMSG_DEF	10
#define SHADOW_MQ_MAXMSG_MAX	1024

/*
 * Flags for shadow-backed mq open mode.
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

#endif /* _UAPI_SHADOW_MQUEUE_H */
