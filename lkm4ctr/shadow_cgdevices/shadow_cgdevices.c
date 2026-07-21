// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_cgdevices - simulated cgroup device controller
 *
 * The lkm4ctr.ko cgroup-device compatibility subsystem retains the
 * transparent device-open hook points used by the lkm4ctr family on kernels without
 * CONFIG_CGROUP_DEVICE.
 *
 * The old misc-device/ioctl control plane has been removed. With no remaining
 * userspace-visible rule/configuration surface,
 * this module now only installs the transparent open hooks and otherwise
 * preserves native behaviour.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>

#include "shadow_hook.h"

#define SHADOW_CGDEVICES_VERSION "2.0"

static int (*real_chrdev_open)(struct inode *inode, struct file *filp);
static int (*real_blkdev_open)(struct inode *inode, struct file *filp);

static int shadow_chrdev_open(struct inode *inode, struct file *filp);
static int shadow_blkdev_open(struct inode *inode, struct file *filp);

static const char * const cgdev_chrdev_open_names[] = {
	"chrdev_open",
	NULL,
};

/*
 * Block-device open symbol naming has drifted more across kernels than the
 * character-device path. Only same-prototype candidates are safe here, so
 * block enforcement remains best-effort.
 */
static const char * const cgdev_blkdev_open_names[] = {
	"blkdev_open",
	NULL,
};

static struct shadow_hook cgdev_chrdev_open_hook =
	SHADOW_HOOK(cgdev_chrdev_open_names, shadow_chrdev_open, &real_chrdev_open);
static struct shadow_hook cgdev_blkdev_open_hook =
	SHADOW_HOOK(cgdev_blkdev_open_names, shadow_blkdev_open, &real_blkdev_open);
static struct shadow_hook *cgdev_hooks[] = {
	&cgdev_chrdev_open_hook,
	&cgdev_blkdev_open_hook,
	NULL,
};

static int shadow_chrdev_open(struct inode *inode, struct file *filp)
{
	return real_chrdev_open(inode, filp);
}

static int shadow_blkdev_open(struct inode *inode, struct file *filp)
{
	return real_blkdev_open(inode, filp);
}

int shadow_cgdevices_init(void)
{
	int hooked;

	pr_info("shadow_cgdevices: init: installing transparent device-open hooks\n");
	hooked = shadow_hook_install_all(cgdev_hooks, "shadow_cgdevices");
	if (hooked < 0) {
		pr_err("shadow_cgdevices: init: shadow_hook_install_all() failed: %d\n",
		       hooked);
		shadow_hook_remove_all(cgdev_hooks);
		return hooked;
	}
	pr_info("shadow_cgdevices: init: %d hook(s) installed\n", hooked);
	pr_info("shadow_cgdevices: loaded with transparent device-open hooks only\n");
	return 0;
}

void shadow_cgdevices_exit(void)
{
	pr_info("shadow_cgdevices: exit: removing transparent device-open hooks\n");
	shadow_hook_remove_all(cgdev_hooks);
	pr_info("shadow_cgdevices: unloaded\n");
}

/*
 * Presence marker for lkm4ctr_checker (see shadow_sysvipc.c for rationale).
 */
int shadow_cgdevices_is_active(void)
{
	return 1;
}
EXPORT_SYMBOL_GPL(shadow_cgdevices_is_active);
