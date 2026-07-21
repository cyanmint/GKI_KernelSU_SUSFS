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

static int __init lkm4ctr_init(void)
{
	int ret;

	ret = shadow_hijack_init();
	if (ret)
		return ret;

	ret = shadow_ns_init();
	if (ret)
		goto err_ns;

	ret = shadow_sysvipc_init();
	if (ret)
		goto err_sysvipc;

	ret = shadow_mqueue_init();
	if (ret)
		goto err_mqueue;

	ret = shadow_cgdevices_init();
	if (ret)
		goto err_cgdevices;

	ret = lkm4ctr_diagfs_init();
	if (ret) {
		LKM4CTR_WARN(LKM4CTR_TAG,
			     "diagfs registration failed: %d (mount -t lkm4ctr, including safe_unload, will be unavailable)",
			     ret);
	}

	LKM4CTR_INFO(LKM4CTR_TAG, "loaded unified module");
	return 0;

err_cgdevices:
	shadow_mqueue_exit();
err_mqueue:
	shadow_sysvipc_exit();
err_sysvipc:
	shadow_ns_exit();
err_ns:
	shadow_hijack_exit();
	return ret;
}

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
