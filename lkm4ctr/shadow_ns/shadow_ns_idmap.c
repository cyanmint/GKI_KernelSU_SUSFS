// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns real uid_map/gid_map table.
 *
 * Before this file existed, shadow_ns_user.c simulated CLONE_NEWUSER with a
 * single hardcoded rule: "namespace uid/gid 0 is real_uid/real_gid, nothing
 * else exists" -- fine for the single-mapping docker/runc default
 * ("0 100000 65536" via a single newuidmap(1) line collapsed to just its
 * first entry), but not a real uid_map/gid_map: multi-entry idmaps (e.g.
 * rootless podman's per-user /etc/subuid ranges, or any container runtime
 * that maps more than one contiguous id range) had no way to be represented,
 * and nothing at all backed actual newuidmap(1)/newgidmap(1) writes.
 *
 * This file adds the real, multi-extent table kernel/user_namespace.c's
 * struct uid_gid_map represents (minus its rbtree fallback for very large
 * maps -- newuidmap/newgidmap line counts are always small in practice, so
 * a linear scan is the same actual cost the real kernel's small-map fast
 * path already uses) plus map_write()'s validation rules, so
 * /proc/<pid>/uid_map and gid_map (see shadow_ns_procfs.c) can expose and
 * accept genuine multi-entry maps.
 */
#include "shadow_ns_internal.h"

/* Real kernel's overflowuid/overflowgid (kernel/user_namespace.c calls these
 * the same thing): the id a lookup with no matching extent falls back to.
 */
#define SHADOW_NS_OVERFLOWID	65534U
#define SHADOW_NS_ID_MAX_EXTENTS	64

static bool shadow_ns_idmap_ranges_overlap(u32 a_first, u32 a_count,
					   u32 b_first, u32 b_count)
{
	u32 a_last = a_first + a_count - 1;
	u32 b_last = b_first + b_count - 1;

	return a_first <= b_last && b_first <= a_last;
}

ssize_t shadow_ns_idmap_write(struct shadow_id_map *map, const char *buf, size_t len)
{
	struct shadow_id_map_extent *extents;
	unsigned int nr = 0, cap = 8, i;
	size_t pos = 0;
	ssize_t ret;

	mutex_lock(&map->lock);
	if (map->extents) {
		/* Real uid_map/gid_map may only be written once. */
		ret = -EPERM;
		goto out_unlock;
	}

	extents = kmalloc_array(cap, sizeof(*extents), GFP_KERNEL);
	if (!extents) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	while (pos < len) {
		unsigned int first, lower_first, count;
		int n, consumed;

		while (pos < len && (buf[pos] == '\n' || buf[pos] == ' ' ||
				      buf[pos] == '\t'))
			pos++;
		if (pos >= len)
			break;

		n = sscanf(buf + pos, "%u %u %u%n", &first, &lower_first,
			   &count, &consumed);
		if (n != 3) {
			ret = -EINVAL;
			goto out_free;
		}
		pos += consumed;

		/*
		 * An extent covers the inclusive id range [x, x+count-1], so
		 * the largest valid count for a given start x is
		 * (U32_MAX - x + 1), i.e. this rejects only once x+count
		 * would exceed U32_MAX+1 -- x == U32_MAX, count == 1 (last
		 * id == U32_MAX exactly) is the boundary case that must
		 * still be accepted, and is: (u64)U32_MAX + 1 is not
		 * itself > (u64)U32_MAX + 1.
		 */
		if (count == 0 ||
		    (u64)first + count > (u64)U32_MAX + 1 ||
		    (u64)lower_first + count > (u64)U32_MAX + 1) {
			ret = -EINVAL;
			goto out_free;
		}

		for (i = 0; i < nr; i++) {
			if (shadow_ns_idmap_ranges_overlap(extents[i].first,
							   extents[i].count,
							   first, count) ||
			    shadow_ns_idmap_ranges_overlap(
				    extents[i].lower_first, extents[i].count,
				    lower_first, count)) {
				ret = -EINVAL;
				goto out_free;
			}
		}

		if (nr == cap) {
			struct shadow_id_map_extent *grown;

			if (cap >= SHADOW_NS_ID_MAX_EXTENTS) {
				ret = -EINVAL;
				goto out_free;
			}
			cap *= 2;
			grown = krealloc(extents, cap * sizeof(*extents), GFP_KERNEL);
			if (!grown) {
				ret = -ENOMEM;
				goto out_free;
			}
			extents = grown;
		}

		extents[nr].first = first;
		extents[nr].lower_first = lower_first;
		extents[nr].count = count;
		nr++;
	}

	if (!nr) {
		ret = -EINVAL;
		goto out_free;
	}

	map->extents = extents;
	map->nr_extents = nr;
	mutex_unlock(&map->lock);
	return (ssize_t)len;

out_free:
	kfree(extents);
out_unlock:
	mutex_unlock(&map->lock);
	return ret;
}

size_t shadow_ns_idmap_format(struct shadow_id_map *map, char *buf, size_t buflen)
{
	size_t off = 0;
	unsigned int i;

	mutex_lock(&map->lock);
	if (!map->extents) {
		off += scnprintf(buf + off, buflen - off, "%9u %9u %9u\n",
				  0U, map->fallback_lower_first, 1U);
		goto out;
	}

	for (i = 0; i < map->nr_extents && off < buflen; i++)
		off += scnprintf(buf + off, buflen - off, "%9u %9u %9u\n",
				  map->extents[i].first,
				  map->extents[i].lower_first,
				  map->extents[i].count);
out:
	mutex_unlock(&map->lock);
	return off;
}

u32 shadow_ns_idmap_translate_up(struct shadow_id_map *map, u32 id, bool *found)
{
	unsigned int i;
	u32 ret = SHADOW_NS_OVERFLOWID;

	mutex_lock(&map->lock);
	if (!map->extents) {
		if (id == 0) {
			ret = map->fallback_lower_first;
			if (found)
				*found = true;
		} else if (found) {
			*found = false;
		}
		goto out;
	}

	if (found)
		*found = false;
	for (i = 0; i < map->nr_extents; i++) {
		struct shadow_id_map_extent *e = &map->extents[i];

		if (id >= e->first && id < e->first + e->count) {
			ret = e->lower_first + (id - e->first);
			if (found)
				*found = true;
			break;
		}
	}
out:
	mutex_unlock(&map->lock);
	return ret;
}

u32 shadow_ns_idmap_translate_down(struct shadow_id_map *map, u32 id, bool *found)
{
	unsigned int i;
	u32 ret = SHADOW_NS_OVERFLOWID;

	mutex_lock(&map->lock);
	if (!map->extents) {
		if (id == map->fallback_lower_first) {
			ret = 0;
			if (found)
				*found = true;
		} else if (found) {
			*found = false;
		}
		goto out;
	}

	if (found)
		*found = false;
	for (i = 0; i < map->nr_extents; i++) {
		struct shadow_id_map_extent *e = &map->extents[i];

		if (id >= e->lower_first && id < e->lower_first + e->count) {
			ret = e->first + (id - e->lower_first);
			if (found)
				*found = true;
			break;
		}
	}
out:
	mutex_unlock(&map->lock);
	return ret;
}
