// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ctr - combined shadow container-support kernel module.
 *
 * This is the top-level glue for the merged "shadow_ctr.ko" module, which
 * links together (as separate translation units, each still independently
 * documented in its own README under ctr_patches/):
 *
 *   - shadow_ns.c          transparent namespace simulation (unshare/setns/
 *                           clone/fork/hostname hijacking)
 *   - shadow_sysvipc.c      transparent System V IPC simulation (msgget/
 *                           semget/shmget/msgctl/semctl/shmctl hijacking)
 *   - shadow_mqueue.c        transparent POSIX message queue simulation
 *                           (mq_open/mq_send/mq_receive/... hijacking)
 *   - shadow_cgdevices.c    transparent cgroup device-access enforcement
 *                           (chrdev_open/blkdev_open hijacking)
 *   - shadow_configspoof.c  /proc/config.gz spoofing overlay
 *
 * Each of the above files retains its own /dev/shadow_* misc device, ioctl
 * ABI, and hooking logic (via the shared shadow_hook.h helper), but exposes
 * a plain (non-static) init/exit function pair rather than its own
 * module_init()/module_exit(): the Linux module loader only allows a single
 * init_module()/cleanup_module() alias per linked .ko (those macros create
 * such an alias directly), so only *this* file may invoke module_init()/
 * module_exit(), and it does so by explicitly calling each subsystem's
 * init/exit in a fixed order, unwinding on failure. This file only carries
 * the module-wide metadata (once, rather than duplicated five times) and
 * the combined init/exit sequencing plus a single dmesg banner.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>

#include "shadow_ctr_internal.h"

static int __init shadow_ctr_main_init(void)
{
	int ret;

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

	ret = shadow_configspoof_init();
	if (ret)
		goto err_configspoof;

	pr_info("shadow_ctr: combined shadow container-support module loaded (shadow_ns + shadow_sysvipc + shadow_mqueue + shadow_cgdevices + shadow_configspoof)\n");
	return 0;

err_configspoof:
	shadow_cgdevices_exit();
err_cgdevices:
	shadow_mqueue_exit();
err_mqueue:
	shadow_sysvipc_exit();
err_sysvipc:
	shadow_ns_exit();
err_ns:
	pr_err("shadow_ctr: failed to initialise (%d), unwound all subsystems\n", ret);
	return ret;
}

static void __exit shadow_ctr_main_exit(void)
{
	/* Tear down in the reverse order of initialisation. */
	shadow_configspoof_exit();
	shadow_cgdevices_exit();
	shadow_mqueue_exit();
	shadow_sysvipc_exit();
	shadow_ns_exit();

	pr_info("shadow_ctr: combined shadow container-support module unloaded\n");
}

module_init(shadow_ctr_main_init);
module_exit(shadow_ctr_main_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Combined shadow container-support module: transparent namespace/sysvipc/mqueue/cgdevices hijacking + /proc/config.gz spoofing");
MODULE_VERSION("2.0");
