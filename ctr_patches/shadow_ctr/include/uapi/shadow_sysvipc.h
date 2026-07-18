/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_sysvipc - simulated System V IPC subsystem UAPI
 *
 * Stable ioctl ABI shared between the shadow_sysvipc kernel module and
 * userspace clients that choose to talk to /dev/shadow_sysvipc directly.
 *
 * On kernels built without CONFIG_SYSVIPC the msgget/semget/shmget family of
 * syscalls return ENOSYS.  This module provides an out-of-tree simulation that
 * can be reached either transparently via ftrace-hooked syscall wrappers or
 * explicitly through ioctls on /dev/shadow_sysvipc to track virtual SysV IPC
 * resources without needing in-kernel SysV support.
 *
 * See ctr_patches/shadow_sysvipc/README.md for design and limitations.
 */
#ifndef _UAPI_SHADOW_SYSVIPC_H
#define _UAPI_SHADOW_SYSVIPC_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SHADOW_SYSVIPC_DEVICE_NAME	"shadow_sysvipc"
#define SHADOW_SYSVIPC_DEVICE_PATH	"/dev/shadow_sysvipc"

/* Bump whenever the ioctl ABI below changes incompatibly. */
#define SHADOW_SYSVIPC_ABI_VERSION	1

/*
 * Virtual IPC resource types.  Values mirror the three classic SysV IPC object
 * kinds so transparent syscall hooks and explicit ioctl clients can both map
 * IPC_* operations directly.
 */
enum shadow_sysvipc_type {
	SHADOW_SYSVIPC_TYPE_MSGQ = 0,	/* message queue  */
	SHADOW_SYSVIPC_TYPE_SEM  = 1,	/* semaphore set  */
	SHADOW_SYSVIPC_TYPE_SHM  = 2,	/* shared-memory segment */
	SHADOW_SYSVIPC_TYPE_MAX  = 3,
};

/*
 * Flags for shadow_sysvipc_create.flags.
 * Values deliberately match the standard Linux IPC_CREAT / IPC_EXCL values.
 */
#define SHADOW_IPC_CREAT	0x0200
#define SHADOW_IPC_EXCL		0x0400
/* Key value that requests a private (non-keyed) resource. */
#define SHADOW_IPC_PRIVATE	((int)0)

/*
 * struct shadow_sysvipc_create - create or get a virtual IPC resource.
 *
 * Inputs:
 *   @type:  one of enum shadow_sysvipc_type
 *   @flags: SHADOW_IPC_CREAT | SHADOW_IPC_EXCL | permission-bits
 *   @key:   SHADOW_IPC_PRIVATE, or an application-chosen integer key
 *   @nsems: number of semaphores (TYPE_SEM only; ignored for other types)
 *   @size:  segment size in bytes (TYPE_SHM only; ignored for other types)
 *
 * Output:
 *   @id:    stable resource id returned to the caller
 *
 * Semantics mirror msgget(2)/semget(2)/shmget(2):
 *   - SHADOW_IPC_PRIVATE always creates a new private resource.
 *   - Non-private key without SHADOW_IPC_CREAT returns ENOENT if not found.
 *   - Non-private key with SHADOW_IPC_CREAT creates if absent, gets if present.
 *   - Non-private key with SHADOW_IPC_CREAT|SHADOW_IPC_EXCL fails with EEXIST
 *     if the key already exists.
 */
struct shadow_sysvipc_create {
	__u32 type;
	__u32 flags;
	__s32 key;
	__u32 nsems;
	__u64 size;
	__u32 id;
	__u32 _pad;
};

/*
 * struct shadow_sysvipc_stat - query metadata about a virtual IPC resource.
 *
 * Input:  @type, @id
 * Output: @key, @flags, @nsems, @size
 */
struct shadow_sysvipc_stat {
	__u32 type;
	__u32 id;
	__s32 key;
	__u32 flags;
	__u32 nsems;
	__u32 _pad;
	__u64 size;
};

/*
 * struct shadow_sysvipc_destroy - release the session's reference to a
 * resource.  The resource is freed when its last reference is dropped.
 */
struct shadow_sysvipc_destroy {
	__u32 type;
	__u32 id;
};

#define SHADOW_SYSVIPC_IOC_MAGIC	'V'

/* Return the ABI version implemented by the loaded module. */
#define SHADOW_SYSVIPC_IOC_ABI_VERSION \
	_IOR(SHADOW_SYSVIPC_IOC_MAGIC, 0, __u32)
/* Create or get a virtual IPC resource by key. */
#define SHADOW_SYSVIPC_IOC_CREATE \
	_IOWR(SHADOW_SYSVIPC_IOC_MAGIC, 1, struct shadow_sysvipc_create)
/* Query metadata about a virtual IPC resource. */
#define SHADOW_SYSVIPC_IOC_STAT \
	_IOWR(SHADOW_SYSVIPC_IOC_MAGIC, 2, struct shadow_sysvipc_stat)
/* Release the session's ownership reference to a resource. */
#define SHADOW_SYSVIPC_IOC_DESTROY \
	_IOW(SHADOW_SYSVIPC_IOC_MAGIC, 3, struct shadow_sysvipc_destroy)

#endif /* _UAPI_SHADOW_SYSVIPC_H */
