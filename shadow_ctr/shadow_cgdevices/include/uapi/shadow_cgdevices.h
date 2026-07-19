/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_cgdevices - simulated cgroup device controller UAPI
 *
 * Shared cgroup-device rule constants used by shadow_cgdevices. The module now
 * exposes no userspace ioctl ABI.
 */
#ifndef _UAPI_SHADOW_CGDEVICES_H
#define _UAPI_SHADOW_CGDEVICES_H

#include <linux/types.h>

/*
 * Device type codes matching the cgroup v1 device controller convention:
 *   'a' = all devices
 *   'b' = block device
 *   'c' = character device
 */
#define SHADOW_CGDEV_TYPE_ALL	'a'
#define SHADOW_CGDEV_TYPE_BLOCK	'b'
#define SHADOW_CGDEV_TYPE_CHAR	'c'

/* Access permission bits matching the cgroup v1 convention. */
#define SHADOW_CGDEV_READ	0x01	/* r */
#define SHADOW_CGDEV_WRITE	0x02	/* w */
#define SHADOW_CGDEV_MKNOD	0x04	/* m */

#endif /* _UAPI_SHADOW_CGDEVICES_H */
