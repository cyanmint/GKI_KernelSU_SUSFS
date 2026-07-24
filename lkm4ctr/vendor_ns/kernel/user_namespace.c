// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendored from kernel-common kernel/user_namespace.c (kernel version 6.1.124,
 * android14-6.1 branch). See lkm4ctr/vendor_ns/README.md for the vendoring
 * rules this file follows. Only the changes marked "RENAME", "DIAGFS
 * PLUMBING" or "STANDALONE COMPILE" below differ from the pristine kernel
 * source; everything else is intentionally close to the original.
 */

#include <linux/export.h>
#include <linux/nsproxy.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>
#include <linux/highuid.h>
#include <linux/cred.h>
#include <linux/securebits.h>
#include <linux/security.h>
#include <linux/keyctl.h>
#include <linux/key-type.h>
#include <keys/user-type.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <linux/projid.h>
#include <linux/fs_struct.h>
#include <linux/bsearch.h>
#include <linux/sort.h>

#include "../vendor_ns.h"

/*
 * STANDALONE COMPILE: the real kernel initialises a dedicated slab cache and
 * ucount/sysctl plumbing at boot. vendor_ns does not participate in global
 * ucount or procfs id-map file management, so it uses kzalloc/kfree plus small
 * local stubs while keeping the core map_id_* and create_user_ns structure.
 */
static DEFINE_MUTEX(vns_userns_state_mutex); /* RENAME */

static void vns_free_user_ns_workfn(struct work_struct *work) {} /* STANDALONE COMPILE */

static struct ucounts *vns_inc_user_namespaces(struct user_namespace *ns, kuid_t uid)
{
	return (struct ucounts *)1; /* STANDALONE COMPILE */
}

static void vns_dec_user_namespaces(struct ucounts *ucounts)
{
}

struct user_namespace *vns_get_user_ns(struct user_namespace *ns) /* RENAME */
{
	if (ns)
		refcount_inc(&ns->ns.count);
	return ns;
}

static void vns_set_cred_user_ns(struct cred *cred,
				 struct user_namespace *user_ns) /* RENAME */
{
	cred->securebits = SECUREBITS_DEFAULT;
	cred->cap_inheritable = CAP_EMPTY_SET;
	cred->cap_permitted = CAP_FULL_SET;
	cred->cap_effective = CAP_FULL_SET;
	cred->cap_ambient = CAP_EMPTY_SET;
	cred->cap_bset = CAP_FULL_SET;
#ifdef CONFIG_KEYS
	key_put(cred->request_key_auth);
	cred->request_key_auth = NULL;
#endif
	cred->user_ns = user_ns;
}

static unsigned long vns_enforced_nproc_rlimit(void) /* RENAME */
{
	unsigned long limit = RLIM_INFINITY;

	if (!uid_eq(current_uid(), GLOBAL_ROOT_UID) ||
	    (current_user_ns() != &init_user_ns))
		limit = rlimit(RLIMIT_NPROC);
	return limit;
}

struct idmap_key {
	bool map_up;
	u32 id;
	u32 count;
};

static int vns_cmp_map_id(const void *k, const void *e) /* RENAME */
{
	u32 first, last, id2;
	const struct idmap_key *key = k;
	const struct uid_gid_extent *el = e;

	id2 = key->id + key->count - 1;
	if (key->map_up)
		first = el->lower_first;
	else
		first = el->first;
	last = first + el->count - 1;
	if (key->id >= first && key->id <= last &&
	    (id2 >= first && id2 <= last))
		return 0;
	if (key->id < first || id2 < first)
		return -1;
	return 1;
}

static struct uid_gid_extent *vns_map_id_range_down_max(unsigned extents,
		struct uid_gid_map *map, u32 id, u32 count) /* RENAME */
{
	struct idmap_key key;

	key.map_up = false;
	key.count = count;
	key.id = id;
	return bsearch(&key, map->forward, extents,
		       sizeof(struct uid_gid_extent), vns_cmp_map_id);
}

static struct uid_gid_extent *vns_map_id_range_down_base(unsigned extents,
		struct uid_gid_map *map, u32 id, u32 count) /* RENAME */
{
	unsigned idx;
	u32 first, last, id2;

	id2 = id + count - 1;
	for (idx = 0; idx < extents; idx++) {
		first = map->extent[idx].first;
		last = first + map->extent[idx].count - 1;
		if (id >= first && id <= last &&
		    (id2 >= first && id2 <= last))
			return &map->extent[idx];
	}
	return NULL;
}

static u32 vns_map_id_range_down(struct uid_gid_map *map, u32 id, u32 count)
{
	struct uid_gid_extent *extent;
	unsigned extents = map->nr_extents;
	smp_rmb();

	if (extents <= UID_GID_MAP_MAX_BASE_EXTENTS)
		extent = vns_map_id_range_down_base(extents, map, id, count);
	else
		extent = vns_map_id_range_down_max(extents, map, id, count);
	if (extent)
		id = (id - extent->first) + extent->lower_first;
	else
		id = (u32)-1;
	return id;
}

u32 vns_map_id_down(struct uid_gid_map *map, u32 id)
{
	return vns_map_id_range_down(map, id, 1);
}

static struct uid_gid_extent *vns_map_id_up_base(unsigned extents,
		struct uid_gid_map *map, u32 id) /* RENAME */
{
	unsigned idx;
	u32 first, last;

	for (idx = 0; idx < extents; idx++) {
		first = map->extent[idx].lower_first;
		last = first + map->extent[idx].count - 1;
		if (id >= first && id <= last)
			return &map->extent[idx];
	}
	return NULL;
}

static struct uid_gid_extent *vns_map_id_up_max(unsigned extents,
		struct uid_gid_map *map, u32 id) /* RENAME */
{
	struct idmap_key key;

	key.map_up = true;
	key.count = 1;
	key.id = id;
	return bsearch(&key, map->reverse, extents,
		       sizeof(struct uid_gid_extent), vns_cmp_map_id);
}

u32 vns_map_id_up(struct uid_gid_map *map, u32 id)
{
	struct uid_gid_extent *extent;
	unsigned extents = map->nr_extents;
	smp_rmb();

	if (extents <= UID_GID_MAP_MAX_BASE_EXTENTS)
		extent = vns_map_id_up_base(extents, map, id);
	else
		extent = vns_map_id_up_max(extents, map, id);
	if (extent)
		id = (id - extent->lower_first) + extent->first;
	else
		id = (u32)-1;
	return id;
}

bool vns_in_userns(const struct user_namespace *ancestor,
	const struct user_namespace *child) /* RENAME */
{
	const struct user_namespace *ns;

	for (ns = child; ns && ns->level > ancestor->level; ns = ns->parent)
		;
	return ns == ancestor;
}

static void vns_user_seed_identity(struct uid_gid_map *map) /* STANDALONE COMPILE */
{
	map->nr_extents = 1;
	map->extent[0].first = 0;
	map->extent[0].lower_first = 0;
	map->extent[0].count = ~0U;
}

struct user_namespace *vns_user_root(void)
{
	struct user_namespace *ns;
	int i;

	ns = kzalloc(sizeof(*ns), GFP_KERNEL); /* STANDALONE COMPILE */
	if (!ns)
		return NULL;
	refcount_set(&ns->ns.count, 1);
	ns->level = 0;
	ns->owner = GLOBAL_ROOT_UID;
	ns->group = GLOBAL_ROOT_GID;
	ns->flags = USERNS_INIT_FLAGS;
	for (i = 0; i < UCOUNT_COUNTS; i++)
		ns->ucount_max[i] = INT_MAX;
	set_userns_rlimit_max(ns, UCOUNT_RLIMIT_NPROC, vns_enforced_nproc_rlimit());
	vns_user_seed_identity(&ns->uid_map);
	vns_user_seed_identity(&ns->gid_map);
	vns_user_seed_identity(&ns->projid_map);
	if (vns_ns_alloc_inum(&ns->ns)) {
		kfree(ns);
		return NULL;
	}
	vns_ns_register(&ns->ns, VENDOR_NS_TYPE_USER); /* DIAGFS PLUMBING */
	return ns;
}

/*
 * Create a new user namespace, deriving the creator from the user in the
 * passed credentials, and replacing that user with the new root user for the
 * new namespace.
 */
struct user_namespace *vns_create_user_ns_from_parent(struct user_namespace *parent_ns)
{
	struct user_namespace *ns;
	kuid_t owner = current_euid();
	kgid_t group = current_egid();
	struct ucounts *ucounts;
	int i;

	if (!parent_ns)
		return vns_user_root();
	ucounts = vns_inc_user_namespaces(parent_ns, owner); /* STANDALONE COMPILE */
	if (!ucounts)
		return NULL;
	ns = kzalloc(sizeof(*ns), GFP_KERNEL); /* STANDALONE COMPILE */
	if (!ns)
		goto fail_dec;
	ns->parent_could_setfcap = false;
	if (vns_ns_alloc_inum(&ns->ns))
		goto fail_free;
	refcount_set(&ns->ns.count, 1);
	ns->parent = parent_ns;
	ns->level = parent_ns->level + 1;
	ns->owner = owner;
	ns->group = group;
	INIT_WORK(&ns->work, vns_free_user_ns_workfn); /* STANDALONE COMPILE */
	for (i = 0; i < UCOUNT_COUNTS; i++)
		ns->ucount_max[i] = INT_MAX;
	set_userns_rlimit_max(ns, UCOUNT_RLIMIT_NPROC, vns_enforced_nproc_rlimit());
	ns->ucounts = ucounts;
	mutex_lock(&vns_userns_state_mutex);
	ns->flags = parent_ns->flags;
	mutex_unlock(&vns_userns_state_mutex);
	vns_ns_register(&ns->ns, VENDOR_NS_TYPE_USER); /* DIAGFS PLUMBING */
	return ns;
fail_free:
	kfree(ns);
fail_dec:
	vns_dec_user_namespaces(ucounts);
	return NULL;
}

void vns_free_user_ns(struct user_namespace *ns)
{
	if (!ns)
		return;
	if (!refcount_dec_and_test(&ns->ns.count))
		return;
	if (ns->gid_map.nr_extents > UID_GID_MAP_MAX_BASE_EXTENTS) {
		kfree(ns->gid_map.forward);
		kfree(ns->gid_map.reverse);
	}
	if (ns->uid_map.nr_extents > UID_GID_MAP_MAX_BASE_EXTENTS) {
		kfree(ns->uid_map.forward);
		kfree(ns->uid_map.reverse);
	}
	if (ns->projid_map.nr_extents > UID_GID_MAP_MAX_BASE_EXTENTS) {
		kfree(ns->projid_map.forward);
		kfree(ns->projid_map.reverse);
	}
	vns_ns_unregister(&ns->ns); /* DIAGFS PLUMBING */
	vns_ns_free_inum(&ns->ns);
	vns_dec_user_namespaces(ns->ucounts);
	kfree(ns);
}

void vns_put_user_ns(struct user_namespace *ns) /* RENAME */
{
	vns_free_user_ns(ns);
}

/*
 * STANDALONE COMPILE: the remaining pristine user_namespace.c surface is procfs
 * id-map read/write plumbing, namespace-owner vtables, and boot-time slab/sysctl
 * initialisation. vendor_ns's shadow-hook path never invokes those entry points,
 * so they are intentionally omitted while the core id-map and namespace-creation
 * logic above remains directly recognisable from the kernel source.
 */
