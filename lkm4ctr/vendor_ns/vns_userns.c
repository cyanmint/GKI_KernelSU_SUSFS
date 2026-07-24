// SPDX-License-Identifier: GPL-2.0
/*
 * vns_userns.c - vendored user namespace + uid/gid extent maps.
 *
 * Vendored/adapted from kernel/user_namespace.c and
 * include/linux/user_namespace.h (Linux kernel, GPL-2.0). Adapted for
 * out-of-tree module use: symbols renamed under a vns_ prefix so they coexist
 * with the builtin kernel's map_id_down()/map_id_up()/... of the same name.
 *
 * Only the "base" (small) extent fast path of the kernel's id-map lookup is
 * vendored. The kernel keeps up to UID_GID_MAP_MAX_EXTENTS (340) mappings in a
 * separately-allocated, bsearch()-ed sorted array once more than
 * UID_GID_MAP_MAX_BASE_EXTENTS (5) extents exist; a container runtime writes
 * only a handful of extents, so the inline linear scan of
 * map_id_range_down_base()/map_id_range_up_base() (reproduced verbatim below,
 * minus the >5-extent branch) covers every realistic uid_map/gid_map. No other
 * logic changes were made.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uidgid.h>

#include "vendor_ns.h"
#include "../../common/lkm4ctr_log.h"

/*
 * Vendored from map_id_range_down_base() (kernel/user_namespace.c): find the
 * extent that maps [id, id+count) downwards (namespace id -> lower/kernel id).
 */
static struct vns_uid_gid_extent *
vns_map_id_range_down_base(unsigned int extents, struct vns_uid_gid_map *map,
			   u32 id, u32 count)
{
	unsigned int idx;
	u32 first, last, id2;

	id2 = id + count - 1;

	/* Find the matching extent */
	for (idx = 0; idx < extents; idx++) {
		first = map->extent[idx].first;
		last = first + map->extent[idx].count - 1;
		if (id >= first && id <= last &&
		    (id2 >= first && id2 <= last))
			return &map->extent[idx];
	}
	return NULL;
}

/* Vendored from map_id_range_down() (kernel/user_namespace.c), base path only. */
static u32 vns_map_id_range_down(struct vns_uid_gid_map *map, u32 id, u32 count)
{
	struct vns_uid_gid_extent *extent;
	unsigned int extents = map->nr_extents;

	if (extents > VNS_UID_GID_MAP_MAX_EXTENTS)
		extents = VNS_UID_GID_MAP_MAX_EXTENTS;

	extent = vns_map_id_range_down_base(extents, map, id, count);

	/* Map the id or note failure */
	if (extent)
		id = (id - extent->first) + extent->lower_first;
	else
		id = (u32) -1;

	return id;
}

/* Vendored from map_id_down() (kernel/user_namespace.c). */
u32 vns_map_id_down(struct vns_uid_gid_map *map, u32 id)
{
	return vns_map_id_range_down(map, id, 1);
}

/*
 * Vendored from map_id_up_base() (kernel/user_namespace.c, Linux 6.1.124):
 * find the extent that maps @id upwards (lower/kernel id -> namespace id). Note
 * the 6.1 "up" base helper takes a single id (no count), unlike the "down"
 * range helper above -- reproduced faithfully.
 */
static struct vns_uid_gid_extent *
vns_map_id_up_base(unsigned int extents, struct vns_uid_gid_map *map, u32 id)
{
	unsigned int idx;
	u32 first, last;

	/* Find the matching extent */
	for (idx = 0; idx < extents; idx++) {
		first = map->extent[idx].lower_first;
		last = first + map->extent[idx].count - 1;
		if (id >= first && id <= last)
			return &map->extent[idx];
	}
	return NULL;
}

/* Vendored from map_id_up() (kernel/user_namespace.c, Linux 6.1.124), base path only. */
u32 vns_map_id_up(struct vns_uid_gid_map *map, u32 id)
{
	struct vns_uid_gid_extent *extent;
	unsigned int extents = map->nr_extents;

	if (extents > VNS_UID_GID_MAP_MAX_EXTENTS)
		extents = VNS_UID_GID_MAP_MAX_EXTENTS;

	extent = vns_map_id_up_base(extents, map, id);

	/* Map the id or note failure */
	if (extent)
		id = (id - extent->lower_first) + extent->first;
	else
		id = (u32) -1;

	return id;
}

/*
 * The root user namespace has an identity mapping for the whole 32-bit id
 * space, matching the kernel's init_user_ns (which maps every id to itself).
 */
static void vns_user_seed_identity(struct vns_uid_gid_map *map)
{
	map->nr_extents = 1;
	map->extent[0].first = 0;
	map->extent[0].lower_first = 0;
	map->extent[0].count = ~0U;
}

struct vns_user_namespace *vns_user_root(void)
{
	struct vns_user_namespace *ns;

	lockdep_assert_held(&vendor_ns_registry.lock);

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return NULL;

	ns->level = 0;
	ns->parent = NULL;
	ns->owner = GLOBAL_ROOT_UID;
	ns->group = GLOBAL_ROOT_GID;
	vns_user_seed_identity(&ns->uid_map);
	vns_user_seed_identity(&ns->gid_map);
	vns_ns_common_init(&ns->ns, VENDOR_NS_TYPE_USER);
	vns_ns_register(&ns->ns);
	return ns;
}

/*
 * Vendored from create_user_ns() (kernel/user_namespace.c): a fresh child user
 * namespace starts with an empty id map (the runtime later writes uid_map/
 * gid_map) and remembers its parent + nesting level.
 */
struct vns_user_namespace *vns_create_user_ns(struct vns_user_namespace *parent)
{
	struct vns_user_namespace *ns;

	lockdep_assert_held(&vendor_ns_registry.lock);

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return NULL;

	ns->parent = parent;
	ns->level = parent ? parent->level + 1 : 0;
	ns->owner = current_euid();
	ns->group = current_egid();
	/* Empty maps until the runtime writes /proc/<pid>/{uid,gid}_map. */
	ns->uid_map.nr_extents = 0;
	ns->gid_map.nr_extents = 0;
	vns_ns_common_init(&ns->ns, VENDOR_NS_TYPE_USER);
	vns_ns_register(&ns->ns);
	return ns;
}

void vns_free_user_ns(struct vns_user_namespace *ns)
{
	if (!ns)
		return;

	lockdep_assert_held(&vendor_ns_registry.lock);

	if (!refcount_dec_and_test(&ns->ns.count))
		return;

	vns_ns_unregister(&ns->ns);
	kfree(ns);
}
