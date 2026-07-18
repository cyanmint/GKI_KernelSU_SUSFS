/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_cgdevices - simulated cgroup device controller UAPI
 *
 * Stable ABI shared between the shadow_cgdevices kernel module and userspace
 * clients.
 *
 * The Linux cgroup v1 device controller (CONFIG_CGROUP_DEVICE) lets a runtime
 * configure per-container device access policies by writing "allow" / "deny"
 * rules to devices.allow / devices.deny in the cgroup hierarchy. When the
 * controller is absent this module provides a shadow rule store driven through
 * ioctls on /dev/shadow_cgdevices.
 *
 * The ioctl ABI still exposes explicit create/rule/check bookkeeping, but a
 * caller may now also bind the *calling task's real cgroup* to an existing
 * shadow cgroup. Once bound, character-device opens (and best-effort block-
 * device opens on kernels where the symbol can be hooked) are enforced in-
 * kernel without any per-open userspace round trip.
 *
 * See ctr_patches/shadow_cgdevices/README.md for design and limitations.
 */
#ifndef _UAPI_SHADOW_CGDEVICES_H
#define _UAPI_SHADOW_CGDEVICES_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SHADOW_CGDEV_DEVICE_NAME	"shadow_cgdevices"
#define SHADOW_CGDEV_DEVICE_PATH	"/dev/shadow_cgdevices"

/* Bump whenever the ioctl ABI below changes incompatibly. */
#define SHADOW_CGDEV_ABI_VERSION	1

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

/*
 * struct shadow_cgdev_create - create a virtual cgroup.
 *
 * @parent_id: id of the parent shadow cgroup; 0 means the implicit root.
 * @id:        allocated shadow cgroup id (out).
 *
 * The new cgroup inherits no rules from its parent (rules are independent per
 * cgroup, as in cgroup v2). The id is stable for the lifetime of the session
 * that created it.
 */
struct shadow_cgdev_create {
	__u32 parent_id;
	__u32 id;
};

/*
 * struct shadow_cgdev_rule - add an allow or deny rule to a shadow cgroup.
 *
 * Semantics mirror the cgroup v1 devices.allow / devices.deny interface:
 * rules are stored in insertion order; CHECK walks the list and the *last*
 * matching rule wins.
 *
 * @cgroup_id: target shadow cgroup id (in)
 * @dev_type:  SHADOW_CGDEV_TYPE_* (in)
 * @access:    OR of SHADOW_CGDEV_READ / WRITE / MKNOD (in)
 * @allow:     1 = allow rule, 0 = deny rule (in)
 * @major:     device major number; -1 = wildcard (in)
 * @minor:     device minor number; -1 = wildcard (in)
 */
struct shadow_cgdev_rule {
	__u32 cgroup_id;
	__u8  dev_type;
	__u8  access;
	__u8  allow;
	__u8  _pad;
	__s32 major;
	__s32 minor;
};

/*
 * struct shadow_cgdev_bind - bind the caller's real cgroup to a shadow one.
 *
 * @cgroup_id:      shadow cgroup id to bind to the current task's real cgroup.
 *                  Set to 0 to unbind the current real cgroup instead.
 * @flags:          reserved, must be zero for now.
 * @real_cgroup_id: stable kernel cgroup id observed for the caller (out).
 *
 * The binding key is the caller's real default-hierarchy cgroup id, not a
 * userspace-provided pathname. That lets later device opens by any task in the
 * same cgroup be checked transparently inside the kernel.
 */
struct shadow_cgdev_bind {
	__u32 cgroup_id;
	__u32 flags;
	__u64 real_cgroup_id;
};

/*
 * struct shadow_cgdev_check - query whether a device access is permitted.
 *
 * Walks the rule list of the given cgroup. The last matching rule determines
 * the result; if no rule matches the default is deny (0).
 *
 * @cgroup_id: shadow cgroup to query (in)
 * @dev_type:  'b' or 'c' (in; 'a' is not a valid query type)
 * @access:    OR of SHADOW_CGDEV_READ / WRITE / MKNOD (in)
 * @allowed:   1 = access permitted, 0 = access denied (out)
 * @major:     device major number (in)
 * @minor:     device minor number (in)
 */
struct shadow_cgdev_check {
	__u32 cgroup_id;
	__u8  dev_type;
	__u8  access;
	__u8  allowed;
	__u8  _pad;
	__s32 major;
	__s32 minor;
};

#define SHADOW_CGDEV_IOC_MAGIC		'G'

/* Return the ABI version implemented by the loaded module. */
#define SHADOW_CGDEV_IOC_ABI_VERSION \
	_IOR(SHADOW_CGDEV_IOC_MAGIC, 0, __u32)
/* Create a new virtual shadow cgroup. */
#define SHADOW_CGDEV_IOC_CREATE \
	_IOWR(SHADOW_CGDEV_IOC_MAGIC, 1, struct shadow_cgdev_create)
/* Destroy a shadow cgroup owned by this session. */
#define SHADOW_CGDEV_IOC_DESTROY \
	_IOW(SHADOW_CGDEV_IOC_MAGIC, 2, __u32)
/* Append an allow/deny rule to a shadow cgroup's rule list. */
#define SHADOW_CGDEV_IOC_RULE_ADD \
	_IOW(SHADOW_CGDEV_IOC_MAGIC, 3, struct shadow_cgdev_rule)
/* Remove all rules from a shadow cgroup (reset to deny-all). */
#define SHADOW_CGDEV_IOC_RULE_RESET \
	_IOW(SHADOW_CGDEV_IOC_MAGIC, 4, __u32)
/* Check whether a device access is permitted by the cgroup's rules. */
#define SHADOW_CGDEV_IOC_CHECK \
	_IOWR(SHADOW_CGDEV_IOC_MAGIC, 5, struct shadow_cgdev_check)
/* Bind/unbind the caller's real cgroup to/from a shadow cgroup. */
#define SHADOW_CGDEV_IOC_BIND \
	_IOWR(SHADOW_CGDEV_IOC_MAGIC, 6, struct shadow_cgdev_bind)

#endif /* _UAPI_SHADOW_CGDEVICES_H */
