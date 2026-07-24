/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vendor_ns - fully vendored namespace subsystem UAPI
 *
 * Shared namespace-type constants and UTS payload layout used by vendor_ns.
 *
 * Unlike shadow_ns (which only simulates a namespace type the running kernel
 * genuinely lacks), vendor_ns hooks every namespace entry point
 * *unconditionally* and manages its own vendored namespace bookkeeping for
 * every type regardless of what the host kernel supports natively. The two
 * subsystems are mutually exclusive at runtime (see lkm4ctr/vendor_ns/README.md
 * and lkm4ctr/lkm4ctr_diagfs.c).
 *
 * vendor_ns exposes no userspace ioctl ABI; only the transparent syscall-hook
 * path and the diagfs introspection tree exist.
 */
#ifndef _UAPI_VENDOR_NS_H
#define _UAPI_VENDOR_NS_H

#include <linux/types.h>

/*
 * Vendored namespace types. The first seven values deliberately match the
 * numeric ordering of shadow_ns's enum shadow_ns_type so the shared diagfs
 * plumbing can key both subsystems' per-type tables off the same integers.
 * vendor_ns adds VENDOR_NS_TYPE_TIME (the time namespace), which shadow_ns
 * does not model.
 */
enum vendor_ns_type {
	VENDOR_NS_TYPE_UTS	= 0,
	VENDOR_NS_TYPE_IPC	= 1,
	VENDOR_NS_TYPE_MNT	= 2,
	VENDOR_NS_TYPE_PID	= 3,
	VENDOR_NS_TYPE_NET	= 4,
	VENDOR_NS_TYPE_USER	= 5,
	VENDOR_NS_TYPE_CGROUP	= 6,
	VENDOR_NS_TYPE_TIME	= 7,
	VENDOR_NS_TYPE_MAX	= 8,
};

/* UTS field length matches the kernel's __NEW_UTS_LEN (64) + NUL. */
#define VENDOR_NS_UTS_LEN	64

/*
 * struct vendor_ns_uts - UTS namespace payload (nodename/domainname).
 *
 * Used internally by vendor_ns's vendored per-namespace UTS storage. Strings
 * are NUL-terminated; the kernel truncates to VENDOR_NS_UTS_LEN characters.
 */
struct vendor_ns_uts {
	char nodename[VENDOR_NS_UTS_LEN + 1];
	char domainname[VENDOR_NS_UTS_LEN + 1];
};

#endif /* _UAPI_VENDOR_NS_H */
