/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadowns - simulated ("shadow") namespace subsystem UAPI
 *
 * This header defines the stable ioctl ABI shared between the shadowns kernel
 * module and userspace clients.
 *
 * The shadow namespace system is an out-of-tree simulation of the kernel
 * namespace model. The module now has both transparent syscall hooks and this
 * explicit ioctl interface: stock userspace can drive the shadow model through
 * unshare(2)/setns(2)/clone(2) and the hostname-related syscalls, while
 * /dev/shadowns remains available for diagnostics and manual control.
 *
 * See ctr_patches/shadowns/README.md for the design and scope/limitations.
 */
#ifndef _UAPI_SHADOWNS_H
#define _UAPI_SHADOWNS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SHADOWNS_DEVICE_NAME	"shadowns"
#define SHADOWNS_DEVICE_PATH	"/dev/shadowns"

/* Bump whenever the ioctl ABI below changes incompatibly. */
#define SHADOWNS_ABI_VERSION	1

/*
 * Shadow namespace types. These mirror the native namespace kinds so a patched
 * runtime can map each requested clone flag onto a shadow namespace type.
 */
enum shadowns_type {
	SHADOWNS_TYPE_UTS	= 0,
	SHADOWNS_TYPE_IPC	= 1,
	SHADOWNS_TYPE_MNT	= 2,
	SHADOWNS_TYPE_PID	= 3,
	SHADOWNS_TYPE_NET	= 4,
	SHADOWNS_TYPE_USER	= 5,
	SHADOWNS_TYPE_CGROUP	= 6,
	SHADOWNS_TYPE_MAX	= 7,
};

/* UTS field length matches the kernel's __NEW_UTS_LEN (64) + NUL. */
#define SHADOWNS_UTS_LEN	64

/*
 * struct shadowns_create - create/unshare a shadow namespace.
 * @type:      one of enum shadowns_type (input)
 * @flags:     reserved, must be 0 (input)
 * @id:        allocated shadow namespace id (output)
 * @parent_id: id of the namespace this one was cloned from, or 0 (output)
 *
 * Used by both SHADOWNS_IOC_CREATE (detached namespace, not joined) and
 * SHADOWNS_IOC_UNSHARE (create a new namespace of @type derived from the
 * session's current namespace of that type, then join it).
 */
struct shadowns_create {
	__u32 type;
	__u32 flags;
	__u32 id;
	__u32 parent_id;
};

/*
 * struct shadowns_setns - join an existing shadow namespace by id.
 * @id:   shadow namespace id to join (input)
 * @type: expected enum shadowns_type, or SHADOWNS_TYPE_MAX to accept any
 *        (input, validated against the target namespace)
 */
struct shadowns_setns {
	__u32 id;
	__u32 type;
};

/*
 * struct shadowns_get - query the session's current namespace of a type.
 * @type: one of enum shadowns_type (input)
 * @id:   current shadow namespace id for @type, or 0 if none (output)
 */
struct shadowns_get {
	__u32 type;
	__u32 id;
};

/*
 * struct shadowns_uts - UTS namespace payload (nodename/domainname).
 *
 * Operates on the session's current SHADOWNS_TYPE_UTS namespace. Strings are
 * NUL-terminated; the kernel truncates to SHADOWNS_UTS_LEN characters.
 */
struct shadowns_uts {
	char nodename[SHADOWNS_UTS_LEN + 1];
	char domainname[SHADOWNS_UTS_LEN + 1];
};

#define SHADOWNS_IOC_MAGIC	'S'

/* Return the ABI version implemented by the module. */
#define SHADOWNS_IOC_ABI_VERSION	_IOR(SHADOWNS_IOC_MAGIC, 0, __u32)
/* Create a detached shadow namespace (does not change current membership). */
#define SHADOWNS_IOC_CREATE		_IOWR(SHADOWNS_IOC_MAGIC, 1, struct shadowns_create)
/* Create a new shadow namespace derived from current and join it. */
#define SHADOWNS_IOC_UNSHARE		_IOWR(SHADOWNS_IOC_MAGIC, 2, struct shadowns_create)
/* Join an existing shadow namespace by id. */
#define SHADOWNS_IOC_SETNS		_IOW(SHADOWNS_IOC_MAGIC, 3, struct shadowns_setns)
/* Query the session's current namespace id for a type. */
#define SHADOWNS_IOC_GET		_IOWR(SHADOWNS_IOC_MAGIC, 4, struct shadowns_get)
/* Drop the caller's reference to a shadow namespace id. */
#define SHADOWNS_IOC_DESTROY		_IOW(SHADOWNS_IOC_MAGIC, 5, __u32)
/* Set nodename/domainname on the current UTS shadow namespace. */
#define SHADOWNS_IOC_SET_UTS		_IOW(SHADOWNS_IOC_MAGIC, 6, struct shadowns_uts)
/* Read nodename/domainname from the current UTS shadow namespace. */
#define SHADOWNS_IOC_GET_UTS		_IOR(SHADOWNS_IOC_MAGIC, 7, struct shadowns_uts)

#endif /* _UAPI_SHADOWNS_H */
