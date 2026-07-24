// SPDX-License-Identifier: GPL-2.0
/*
 * vns_nsfs.c - vendored namespace-inode (ns_common) plumbing.
 *
 * Vendored/adapted from fs/nsfs.c and include/linux/ns_common.h (Linux kernel,
 * GPL-2.0), plus the proc_alloc_inum() inode-number allocator from
 * fs/proc/generic.c. Adapted for out-of-tree module use: every symbol is
 * renamed under a vns_ prefix so the vendored copies coexist with the real
 * builtin kernel symbols of the same name (vendor_ns hooks unconditionally, so
 * both run simultaneously). No algorithmic changes were made beyond the
 * renaming and the registry linkage needed to surface the live namespaces via
 * this module's diagfs files.
 *
 * In the kernel, ns_alloc_inum(struct ns_common *ns) fills ns->inum from
 * proc_alloc_inum() and initialises ns->count to 1; nsfs stashes a dentry per
 * namespace so /proc/<pid>/ns/<type> can be read. vendor_ns keeps only the
 * inode-number allocation and refcount, which is all the vendored bookkeeping
 * needs.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>

#include "vendor_ns.h"
#include "../../common/lkm4ctr_log.h"

/*
 * The kernel's namespace inode numbers come from proc_alloc_inum(), which hands
 * out values from an ida biased above PROC_DYNAMIC_FIRST (0xF0000000). We mirror
 * that starting point so the fabricated /proc/<pid>/ns/<type> inode numbers land
 * in the same numeric neighbourhood real namespace inodes occupy, then allocate
 * cyclically under vendor_ns_registry.lock.
 */
#define VNS_PROC_DYNAMIC_FIRST	0xF0000000U

unsigned int vns_alloc_inum(void)
{
	unsigned int inum;

	lockdep_assert_held(&vendor_ns_registry.lock);

	if (vendor_ns_registry.next_inum < VNS_PROC_DYNAMIC_FIRST)
		vendor_ns_registry.next_inum = VNS_PROC_DYNAMIC_FIRST;

	inum = vendor_ns_registry.next_inum++;
	if (vendor_ns_registry.next_inum == 0)
		vendor_ns_registry.next_inum = VNS_PROC_DYNAMIC_FIRST;

	return inum;
}

void vns_ns_common_init(struct vns_ns_common *ns, u32 type)
{
	ns->inum = vns_alloc_inum();
	refcount_set(&ns->count, 1);
	ns->type = type;
	INIT_LIST_HEAD(&ns->registry);
}

void vns_ns_register(struct vns_ns_common *ns)
{
	lockdep_assert_held(&vendor_ns_registry.lock);

	if (ns->type >= VENDOR_NS_TYPE_MAX)
		return;

	list_add_tail(&ns->registry, &vendor_ns_registry.ns_list[ns->type]);
	vendor_ns_registry.ns_count[ns->type]++;
	vendor_ns_registry.ns_created[ns->type]++;
}

void vns_ns_unregister(struct vns_ns_common *ns)
{
	lockdep_assert_held(&vendor_ns_registry.lock);

	if (ns->type >= VENDOR_NS_TYPE_MAX)
		return;
	if (list_empty(&ns->registry))
		return;

	list_del_init(&ns->registry);
	if (vendor_ns_registry.ns_count[ns->type])
		vendor_ns_registry.ns_count[ns->type]--;
}

const char *vns_type_name(u32 type)
{
	switch (type) {
	case VENDOR_NS_TYPE_UTS:	return "uts";
	case VENDOR_NS_TYPE_IPC:	return "ipc";
	case VENDOR_NS_TYPE_MNT:	return "mnt";
	case VENDOR_NS_TYPE_PID:	return "pid";
	case VENDOR_NS_TYPE_NET:	return "net";
	case VENDOR_NS_TYPE_USER:	return "user";
	case VENDOR_NS_TYPE_CGROUP:	return "cgroup";
	case VENDOR_NS_TYPE_TIME:	return "time";
	default:			return "?";
	}
}
