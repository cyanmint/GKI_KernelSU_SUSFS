/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_sysvipc - simulated System V IPC subsystem UAPI
 *
 * Shared shadow SysV IPC constants and metadata structs used by the
 * transparent syscall-hook implementation. The shadow_sysvipc module now
 * exposes no userspace ioctl ABI.
 */
#ifndef _UAPI_SHADOW_SYSVIPC_H
#define _UAPI_SHADOW_SYSVIPC_H

#include <linux/types.h>

/*
 * Virtual IPC resource types.  Values mirror the three classic SysV IPC object
 * kinds so transparent syscall hooks can map IPC_* operations directly.
 */
enum shadow_sysvipc_type {
	SHADOW_SYSVIPC_TYPE_MSGQ = 0,	/* message queue  */
	SHADOW_SYSVIPC_TYPE_SEM  = 1,	/* semaphore set  */
	SHADOW_SYSVIPC_TYPE_SHM  = 2,	/* shared-memory segment */
	SHADOW_SYSVIPC_TYPE_MAX  = 3,
};

/*
 * Flags for shadow-backed resource creation.
 * Values deliberately match the standard Linux IPC_CREAT / IPC_EXCL values.
 */
#define SHADOW_IPC_CREAT	0x0200
#define SHADOW_IPC_EXCL		0x0400
/* Key value that requests a private (non-keyed) resource. */
#define SHADOW_IPC_PRIVATE	((int)0)

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

#endif /* _UAPI_SHADOW_SYSVIPC_H */
