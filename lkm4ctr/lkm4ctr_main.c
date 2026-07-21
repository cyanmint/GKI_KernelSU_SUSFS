// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>

#include "lkm4ctr_log.h"

int shadow_hijack_init(void);
void shadow_hijack_exit(void);
int shadow_ns_init(void);
void shadow_ns_exit(void);
int shadow_sysvipc_init(void);
void shadow_sysvipc_exit(void);
int shadow_mqueue_init(void);
void shadow_mqueue_exit(void);
int shadow_cgdevices_init(void);
void shadow_cgdevices_exit(void);
int lkm4ctr_diagfs_init(void);
void lkm4ctr_diagfs_exit(void);

#define LKM4CTR_VERSION "4.0"
#define LKM4CTR_TAG	"lkm4ctr"

/*
 * No submodule (shadow_ns/shadow_sysvipc/shadow_mqueue/shadow_cgdevices) is
 * auto-loaded at insmod time any more: shadow_hijack_init() is a no-op
 * (shared hook-engine bookkeeping only, no hooks of its own) and
 * lkm4ctr_diagfs_init() only registers the "lkm4ctr" filesystem type, so
 * lkm4ctr.ko now comes up completely passive -- every submodule starts
 * "not loaded" until deliberately started via
 * ./mnt/modules/<name>/status ("echo load"), or all at once via
 * ./mnt/safe_unload ("echo load"). See lkm4ctr_diagfs.c for both.
 */
static int __init lkm4ctr_init(void)
{
	int ret;

	ret = shadow_hijack_init();
	if (ret)
		return ret;

	ret = lkm4ctr_diagfs_init();
	if (ret) {
		LKM4CTR_WARN(LKM4CTR_TAG,
			     "diagfs registration failed: %d (mount -t lkm4ctr, including safe_unload and every submodule's load/unload control, will be unavailable)",
			     ret);
	}

	LKM4CTR_INFO(LKM4CTR_TAG,
		     "loaded unified module (no submodule auto-started; mount -t lkm4ctr diag <mnt> then \"echo load\" to ./mnt/safe_unload or a specific ./mnt/modules/<name>/status)");
	return 0;
}

/*
 * lkm4ctr_exit() unconditionally calls every submodule's own _exit(), which
 * is idempotent-safe and force-frees all of that submodule's resources even
 * if it was never loaded (shadow_hook_remove_all() on hooks that were never
 * installed is a no-op; every submodule's own resource-registry teardown is
 * likewise a no-op on an already-empty registry) or was already manually
 * unloaded via diagfs. This is what guarantees rmmod is never blocked by a
 * submodule's own state: whatever the diagfs left active is force-cleaned
 * up right here, unconditionally, on the way out.
 */
static void __exit lkm4ctr_exit(void)
{
	lkm4ctr_diagfs_exit();
	shadow_cgdevices_exit();
	shadow_mqueue_exit();
	shadow_sysvipc_exit();
	shadow_ns_exit();
	shadow_hijack_exit();
	LKM4CTR_INFO(LKM4CTR_TAG, "unloaded unified module");
}

module_init(lkm4ctr_init);
module_exit(lkm4ctr_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Unified lkm4ctr module: shared hook engine plus namespace, SysV IPC, POSIX mqueue and cgroup-device compatibility subsystems");
MODULE_VERSION(LKM4CTR_VERSION);
