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

#define SHADOW_CTR_VERSION "2.0"

static int __init shadow_ctr_main_init(void)
{
	int ret;

	/*
	 * This is deliberately the very first thing shadow_ctr_main_init()
	 * does. If this line is never seen in dmesg, whatever went wrong
	 * happened before our own module code ever ran (e.g. inside the
	 * kernel's generic module loader -- load_module()/mod_sysfs_setup()
	 * -- while parsing/relocating the .ko itself, such as a vermagic/
	 * module_layout mismatch between the build toolchain/kernel headers
	 * and the running kernel). If it *is* seen, the crash is somewhere
	 * in the init sequence below (or deeper in one of the subsystems),
	 * and the per-subsystem "entering"/"leaving" messages below narrow
	 * it down further.
	 */
	pr_info("shadow_ctr: module init starting (version %s)\n",
		SHADOW_CTR_VERSION);

	pr_info("shadow_ctr: entering shadow_ns_init()\n");
	ret = shadow_ns_init();
	if (ret)
		goto err_ns;
	pr_info("shadow_ctr: shadow_ns_init() succeeded\n");

	pr_info("shadow_ctr: entering shadow_sysvipc_init()\n");
	ret = shadow_sysvipc_init();
	if (ret)
		goto err_sysvipc;
	pr_info("shadow_ctr: shadow_sysvipc_init() succeeded\n");

	pr_info("shadow_ctr: entering shadow_mqueue_init()\n");
	ret = shadow_mqueue_init();
	if (ret)
		goto err_mqueue;
	pr_info("shadow_ctr: shadow_mqueue_init() succeeded\n");

	pr_info("shadow_ctr: entering shadow_cgdevices_init()\n");
	ret = shadow_cgdevices_init();
	if (ret)
		goto err_cgdevices;
	pr_info("shadow_ctr: shadow_cgdevices_init() succeeded\n");

	pr_info("shadow_ctr: entering shadow_configspoof_init()\n");
	ret = shadow_configspoof_init();
	if (ret)
		goto err_configspoof;
	pr_info("shadow_ctr: shadow_configspoof_init() succeeded\n");

	pr_info("shadow_ctr: combined shadow container-support module loaded (shadow_ns + shadow_sysvipc + shadow_mqueue + shadow_cgdevices + shadow_configspoof)\n");
	return 0;

err_configspoof:
	pr_err("shadow_ctr: shadow_configspoof_init() failed (%d), unwinding shadow_cgdevices\n", ret);
	shadow_cgdevices_exit();
err_cgdevices:
	pr_err("shadow_ctr: unwinding shadow_mqueue\n");
	shadow_mqueue_exit();
err_mqueue:
	pr_err("shadow_ctr: unwinding shadow_sysvipc\n");
	shadow_sysvipc_exit();
err_sysvipc:
	pr_err("shadow_ctr: unwinding shadow_ns\n");
	shadow_ns_exit();
err_ns:
	pr_err("shadow_ctr: failed to initialise (%d), unwound all subsystems\n", ret);
	return ret;
}

static void __exit shadow_ctr_main_exit(void)
{
	pr_info("shadow_ctr: module exit starting\n");

	/* Tear down in the reverse order of initialisation. */
	pr_info("shadow_ctr: entering shadow_configspoof_exit()\n");
	shadow_configspoof_exit();
	pr_info("shadow_ctr: entering shadow_cgdevices_exit()\n");
	shadow_cgdevices_exit();
	pr_info("shadow_ctr: entering shadow_mqueue_exit()\n");
	shadow_mqueue_exit();
	pr_info("shadow_ctr: entering shadow_sysvipc_exit()\n");
	shadow_sysvipc_exit();
	pr_info("shadow_ctr: entering shadow_ns_exit()\n");
	shadow_ns_exit();

	pr_info("shadow_ctr: combined shadow container-support module unloaded\n");
}

module_init(shadow_ctr_main_init);
module_exit(shadow_ctr_main_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Combined shadow container-support module: transparent namespace/sysvipc/mqueue/cgdevices hijacking + /proc/config.gz spoofing");
MODULE_VERSION(SHADOW_CTR_VERSION);
