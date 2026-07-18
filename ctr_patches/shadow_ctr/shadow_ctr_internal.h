/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_ctr_internal - cross-TU declarations shared between the five
 * shadow_ctr.ko subsystem source files and shadow_ctr_main.c.
 *
 * The Linux module loader only allows a single init_module()/cleanup_module()
 * alias per linked .ko (module_init()/module_exit() are macros that create
 * those aliases directly), so only shadow_ctr_main.c may call those macros;
 * every other file exposes a plain (non-static) init/exit function pair that
 * shadow_ctr_main.c calls explicitly, in order, with rollback on failure.
 */

#ifndef _SHADOW_CTR_INTERNAL_H
#define _SHADOW_CTR_INTERNAL_H

#include <linux/file.h>

/*
 * fd_file()/fd_empty() were introduced by the "struct fd" API rework
 * (upstream commit "file: convert to struct fd") that landed in v6.8;
 * on the older GKI branches (e.g. 6.1) this module still targets,
 * "struct fd" is a plain aggregate with a directly accessible ->file
 * member, so provide compatible shims when the helpers aren't present.
 */
#ifndef fd_file
#define fd_file(f) ((f).file)
#endif
#ifndef fd_empty
#define fd_empty(f) (!fd_file(f))
#endif

int __init shadow_ns_init(void);
void shadow_ns_exit(void);

int __init shadow_sysvipc_init(void);
void shadow_sysvipc_exit(void);

int __init shadow_mqueue_init(void);
void shadow_mqueue_exit(void);

int __init shadow_cgdevices_init(void);
void shadow_cgdevices_exit(void);

int __init shadow_configspoof_init(void);
void shadow_configspoof_exit(void);

/*
 * shadow_overlay_{init,exit}() (overlay/super.c) provide the real,
 * vendored-from-upstream `fs/overlayfs` "overlay" filesystem type — unlike
 * the subsystems above, this isn't a simulation of kernel behaviour missing
 * from a stock GKI build, it's the genuine implementation, needed because
 * production GKI boot images generally ship with CONFIG_OVERLAY_FS disabled
 * outright (breaking dockerd's overlay2 graphdriver). See
 * overlay/README.md and shadow_configspoof.README.md's CONFIG_OVERLAY_FS
 * note for details. Only linked in when built with WITH_SHADOW_OVERLAY=1
 * (the Makefile's default), since the vendored sources currently only
 * compile against the android14-6.1 kernel branch.
 */
#ifdef SHADOW_CTR_WITH_OVERLAY
int __init shadow_overlay_init(void);
void shadow_overlay_exit(void);
#endif

#endif /* _SHADOW_CTR_INTERNAL_H */
