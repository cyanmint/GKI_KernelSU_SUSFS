/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lkm4ctr_compat - small cross-kernel-version compatibility shims shared
 * by the unified lkm4ctr.ko subsystems.
 *
 * Historically these shims lived in lkm4ctr_internal.h, back when every
 * subsystem was linked into a single combined lkm4ctr.ko. They now live
 * here and are pulled in (via -I../common) by whichever unified-module
 * subsystem actually uses them.
 *
 * fd_file()/fd_empty() were introduced by the "struct fd" API rework
 * (upstream commit "file: convert to struct fd") that landed in v6.8; on the
 * older GKI branches (e.g. 6.1) these modules still target, "struct fd" is a
 * plain aggregate with a directly accessible ->file member, so provide
 * compatible shims when the helpers aren't present. Used by shadow_mqueue
 * and shadow_ns.
 */

#ifndef _LKM4CTR_COMPAT_H
#define _LKM4CTR_COMPAT_H

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/version.h>

#ifndef fd_file
#define fd_file(f) ((f).file)
#endif
#ifndef fd_empty
#define fd_empty(f) (!fd_file(f))
#endif

/*
 * lkm4ctr_inode_init_ts() - set a freshly allocated inode's
 * atime/mtime/ctime to "now", across the API rework simple_inode_init_ts()
 * introduced upstream in v6.6 (older kernels this module targets, down to
 * 5.10, still expose the plain i_atime/i_mtime/i_ctime struct timespec64
 * fields directly). Used by lkm4ctr_diagfs.c's inode allocation helper.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define lkm4ctr_inode_init_ts(inode) simple_inode_init_ts(inode)
#else
#define lkm4ctr_inode_init_ts(inode) \
	((inode)->i_atime = (inode)->i_mtime = (inode)->i_ctime = current_time(inode))
#endif

#endif /* _LKM4CTR_COMPAT_H */
