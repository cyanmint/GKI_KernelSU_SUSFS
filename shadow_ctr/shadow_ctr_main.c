// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>

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

#define SHADOW_CTR_VERSION "4.0"

static int __init shadow_ctr_init(void)
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

	pr_info("shadow_ctr: loaded unified module\n");
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

static void __exit shadow_ctr_exit(void)
{
	shadow_cgdevices_exit();
	shadow_mqueue_exit();
	shadow_sysvipc_exit();
	shadow_ns_exit();
	shadow_hijack_exit();
	pr_info("shadow_ctr: unloaded unified module\n");
}

module_init(shadow_ctr_init);
module_exit(shadow_ctr_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Unified shadow_ctr module: shared hook engine plus namespace, SysV IPC, POSIX mqueue and cgroup-device compatibility subsystems");
MODULE_VERSION(SHADOW_CTR_VERSION);
