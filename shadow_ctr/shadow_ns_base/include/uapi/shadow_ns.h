/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_ns - simulated ("shadow") namespace subsystem UAPI
 *
 * This header defines the stable ioctl ABI shared between the shadow_ns kernel
 * module and userspace clients.
 *
 * The shadow namespace system is an out-of-tree simulation of the kernel
 * namespace model. The module now has both transparent syscall hooks and this
 * explicit ioctl interface: stock userspace can drive the shadow model through
 * unshare(2)/setns(2)/clone(2) and the hostname-related syscalls, while
 * /dev/shadow_ns remains available for diagnostics and manual control.
 *
 * See ctr_patches/shadow_ns/README.md for the design and scope/limitations.
 */
#ifndef _UAPI_SHADOW_NS_H
#define _UAPI_SHADOW_NS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SHADOW_NS_DEVICE_NAME	"shadow_ns"
#define SHADOW_NS_DEVICE_PATH	"/dev/shadow_ns"

/* Bump whenever the ioctl ABI below changes incompatibly. */
#define SHADOW_NS_ABI_VERSION	1

/*
 * Shadow namespace types. These mirror the native namespace kinds so a patched
 * runtime can map each requested clone flag onto a shadow namespace type.
 */
enum shadow_ns_type {
	SHADOW_NS_TYPE_UTS	= 0,
	SHADOW_NS_TYPE_IPC	= 1,
	SHADOW_NS_TYPE_MNT	= 2,
	SHADOW_NS_TYPE_PID	= 3,
	SHADOW_NS_TYPE_NET	= 4,
	SHADOW_NS_TYPE_USER	= 5,
	SHADOW_NS_TYPE_CGROUP	= 6,
	SHADOW_NS_TYPE_MAX	= 7,
};

/* UTS field length matches the kernel's __NEW_UTS_LEN (64) + NUL. */
#define SHADOW_NS_UTS_LEN	64

/*
 * struct shadow_ns_create - create/unshare a shadow namespace.
 * @type:      one of enum shadow_ns_type (input)
 * @flags:     reserved, must be 0 (input)
 * @id:        allocated shadow namespace id (output)
 * @parent_id: id of the namespace this one was cloned from, or 0 (output)
 *
 * Used by both SHADOW_NS_IOC_CREATE (detached namespace, not joined) and
 * SHADOW_NS_IOC_UNSHARE (create a new namespace of @type derived from the
 * session's current namespace of that type, then join it).
 */
struct shadow_ns_create {
	__u32 type;
	__u32 flags;
	__u32 id;
	__u32 parent_id;
};

/*
 * struct shadow_ns_setns - join an existing shadow namespace by id.
 * @id:   shadow namespace id to join (input)
 * @type: expected enum shadow_ns_type, or SHADOW_NS_TYPE_MAX to accept any
 *        (input, validated against the target namespace)
 */
struct shadow_ns_setns {
	__u32 id;
	__u32 type;
};

/*
 * struct shadow_ns_get - query the session's current namespace of a type.
 * @type: one of enum shadow_ns_type (input)
 * @id:   current shadow namespace id for @type, or 0 if none (output)
 */
struct shadow_ns_get {
	__u32 type;
	__u32 id;
};

/*
 * struct shadow_ns_uts - UTS namespace payload (nodename/domainname).
 *
 * Operates on the session's current SHADOW_NS_TYPE_UTS namespace. Strings are
 * NUL-terminated; the kernel truncates to SHADOW_NS_UTS_LEN characters.
 */
struct shadow_ns_uts {
	char nodename[SHADOW_NS_UTS_LEN + 1];
	char domainname[SHADOW_NS_UTS_LEN + 1];
};

#define SHADOW_NS_IOC_MAGIC	'S'

/* Return the ABI version implemented by the module. */
#define SHADOW_NS_IOC_ABI_VERSION	_IOR(SHADOW_NS_IOC_MAGIC, 0, __u32)
/* Create a detached shadow namespace (does not change current membership). */
#define SHADOW_NS_IOC_CREATE		_IOWR(SHADOW_NS_IOC_MAGIC, 1, struct shadow_ns_create)
/* Create a new shadow namespace derived from current and join it. */
#define SHADOW_NS_IOC_UNSHARE		_IOWR(SHADOW_NS_IOC_MAGIC, 2, struct shadow_ns_create)
/* Join an existing shadow namespace by id. */
#define SHADOW_NS_IOC_SETNS		_IOW(SHADOW_NS_IOC_MAGIC, 3, struct shadow_ns_setns)
/* Query the session's current namespace id for a type. */
#define SHADOW_NS_IOC_GET		_IOWR(SHADOW_NS_IOC_MAGIC, 4, struct shadow_ns_get)
/* Drop the caller's reference to a shadow namespace id. */
#define SHADOW_NS_IOC_DESTROY		_IOW(SHADOW_NS_IOC_MAGIC, 5, __u32)
/* Set nodename/domainname on the current UTS shadow namespace. */
#define SHADOW_NS_IOC_SET_UTS		_IOW(SHADOW_NS_IOC_MAGIC, 6, struct shadow_ns_uts)
/* Read nodename/domainname from the current UTS shadow namespace. */
#define SHADOW_NS_IOC_GET_UTS		_IOR(SHADOW_NS_IOC_MAGIC, 7, struct shadow_ns_uts)

#endif /* _UAPI_SHADOW_NS_H */
