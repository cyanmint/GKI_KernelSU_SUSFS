/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_ns - namespace subsystem UAPI
 *
 * Shared namespace-type constants and UTS payload layout used by shadow_ns.
 * shadow_ns exposes no userspace ioctl ABI; only the transparent syscall-hook
 * path exists.
 */
#ifndef _UAPI_SHADOW_NS_H
#define _UAPI_SHADOW_NS_H

#include <linux/types.h>

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
 * struct shadow_ns_uts - UTS namespace payload (nodename/domainname).
 *
 * Used internally by shadow_ns's UTS-simulation fallback for its
 * per-namespace nodename/domainname storage, only ever active on a kernel
 * build that lacks real CONFIG_UTS_NS support. Strings are NUL-terminated;
 * the kernel truncates to SHADOW_NS_UTS_LEN characters.
 */
struct shadow_ns_uts {
	char nodename[SHADOW_NS_UTS_LEN + 1];
	char domainname[SHADOW_NS_UTS_LEN + 1];
};

#endif /* _UAPI_SHADOW_NS_H */
