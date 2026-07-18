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

#endif /* _SHADOW_CTR_INTERNAL_H */
