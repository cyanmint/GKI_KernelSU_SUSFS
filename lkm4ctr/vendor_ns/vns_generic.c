// SPDX-License-Identifier: GPL-2.0
/*
 * vns_generic.c - vendored reference-counted bookkeeping namespaces for the
 * types vendor_ns tracks but does not fully functionally emulate from a
 * loadable module: IPC, MNT, CGROUP, NET and TIME.
 *
 * Modelled directly on the common shape of the corresponding upstream kernel
 * source (all GPL-2.0):
 *   - ipc/namespace.c              (struct ipc_namespace + copy_ipcs)
 *   - fs/namespace.c               (struct mnt_namespace + copy_mnt_ns)
 *   - kernel/cgroup/namespace.c    (struct cgroup_namespace + copy_cgroup_ns)
 *   - net/core/net_namespace.c     (struct net + copy_net_ns)
 *   - kernel/time/namespace.c      (struct time_namespace + copy_time_ns)
 *
 * Every one of those is, at heart, "a payload struct with an embedded struct
 * ns_common", created by a copy_*_ns(flags, ...) that returns the old namespace
 * unchanged unless the matching CLONE_NEW* bit is set, in which case it
 * allocates a new one with a fresh inode number. vendor_ns keeps exactly that
 * shape (vns_ns_common) for these types; the type-specific payloads (IPC id
 * registries, mount trees, cgroup roots, network devices, time offsets) are
 * owned by the real kernel subsystems and are out of reach of a loadable
 * module, which is why these remain reference-counted bookkeeping only. See the
 * README's per-type behaviour table.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "vendor_ns.h"
#include "../../common/lkm4ctr_log.h"

static struct vns_generic_namespace *vns_alloc_generic_ns(u32 type)
{
	struct vns_generic_namespace *ns;

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return NULL;

	vns_ns_common_init(&ns->ns, type);
	vns_ns_register(&ns->ns);
	return ns;
}

struct vns_generic_namespace *vns_generic_root(u32 type)
{
	lockdep_assert_held(&vendor_ns_registry.lock);
	return vns_alloc_generic_ns(type);
}

/* Vendored shape of copy_ipcs()/copy_mnt_ns()/copy_cgroup_ns()/... */
struct vns_generic_namespace *vns_create_generic_ns(u32 type)
{
	lockdep_assert_held(&vendor_ns_registry.lock);
	return vns_alloc_generic_ns(type);
}

void vns_free_generic_ns(struct vns_generic_namespace *ns)
{
	if (!ns)
		return;

	lockdep_assert_held(&vendor_ns_registry.lock);

	if (!refcount_dec_and_test(&ns->ns.count))
		return;

	vns_ns_unregister(&ns->ns);
	kfree(ns);
}
