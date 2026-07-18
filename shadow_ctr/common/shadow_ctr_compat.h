/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_ctr_compat - small cross-kernel-version compatibility shims shared
 * by the standalone shadow_* out-of-tree modules.
 *
 * Historically these shims lived in shadow_ctr_internal.h, back when every
 * subsystem was linked into a single combined shadow_ctr.ko. The module is
 * now split into one independently-loadable .ko per subsystem (see
 * ../README.md), so the shims that are still needed live here and are pulled
 * in (via -I../common) by whichever module actually uses them.
 *
 * fd_file()/fd_empty() were introduced by the "struct fd" API rework
 * (upstream commit "file: convert to struct fd") that landed in v6.8; on the
 * older GKI branches (e.g. 6.1) these modules still target, "struct fd" is a
 * plain aggregate with a directly accessible ->file member, so provide
 * compatible shims when the helpers aren't present. Currently only
 * shadow_mqueue uses these.
 */

#ifndef _SHADOW_CTR_COMPAT_H
#define _SHADOW_CTR_COMPAT_H

#include <linux/file.h>

#ifndef fd_file
#define fd_file(f) ((f).file)
#endif
#ifndef fd_empty
#define fd_empty(f) (!fd_file(f))
#endif

#endif /* _SHADOW_CTR_COMPAT_H */
