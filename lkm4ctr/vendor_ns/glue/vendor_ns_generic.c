// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_ns_generic.c - bookkeeping-only namespace placeholders for IPC/MNT/NET/
 * CGROUP/TIME. These are not direct kernel-source vendoring targets; they are
 * the glue-layer placeholders the task description explicitly allows for the
 * non-vendored namespace types.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "../vendor_ns.h"

static struct vns_generic_namespace *vns_alloc_generic_ns(u32 type)
{
	struct vns_generic_namespace *ns = kzalloc(sizeof(*ns), GFP_KERNEL);

	if (!ns)
		return NULL;
	refcount_set(&ns->ns.count, 1);
	if (vns_ns_alloc_inum(&ns->ns)) {
		kfree(ns);
		return NULL;
	}
	vns_ns_register(&ns->ns, type);
	return ns;
}

struct vns_generic_namespace *vns_generic_root(u32 type)
{
	lockdep_assert_held(&vendor_ns_registry.lock);
	return vns_alloc_generic_ns(type);
}

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
	vns_ns_free_inum(&ns->ns);
	kfree(ns);
}
