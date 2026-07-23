// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns_diag - namespace diagnostics dump for the lkm4ctr diagfs
 * (lkm4ctr_diagfs.c). Read-only introspection over the two registries
 * shadow_ns already maintains (shadow_ns_base.c/shadow_ns_task.c): the flat
 * id -> struct shadow_ns table (shadow_ns_map) and the tgid -> struct
 * shadow_task_group table (shadow_ns_tgid_map) that records each task
 * group's *current* namespace of every type. No new state is introduced
 * here; this only walks the existing ones under their existing locks.
 */
#include "shadow_ns_internal.h"

static const char *shadow_ns_diag_type_name(u32 type)
{
	switch (type) {
	case SHADOW_NS_TYPE_UTS:
		return "uts";
	case SHADOW_NS_TYPE_IPC:
		return "ipc";
	case SHADOW_NS_TYPE_MNT:
		return "mnt";
	case SHADOW_NS_TYPE_PID:
		return "pid";
	case SHADOW_NS_TYPE_NET:
		return "net";
	case SHADOW_NS_TYPE_USER:
		return "user";
	case SHADOW_NS_TYPE_CGROUP:
		return "cgroup";
	default:
		return "?";
	}
}

/*
 * shadow_ns_diag_snprintf_type() - render only namespace objects of one
 * specific @type plus the task groups currently joined to each of them. Used
 * by lkm4ctr_diagfs.c's /ns/<type>/namespaces listing files so the diagfs can
 * expose one directory per namespace type without pretending shadow_ns has a
 * separate load/unload lifecycle per type.
 */
size_t shadow_ns_diag_snprintf_type(u32 type, char *buf, size_t buflen)
{
	size_t pos = 0;
	unsigned long index;
	struct shadow_ns *ns;
	struct shadow_task_group *tg;
	const char *type_name = shadow_ns_diag_type_name(type);

	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			  "type=%s\n", type_name);

	mutex_lock(&shadow_ns_map_lock);
	xa_for_each(&shadow_ns_map, index, ns) {
		if (ns->type != type)
			continue;
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				  "  ns id=%u parent_id=%u refcount=%d\n",
				  ns->id, ns->parent_id, refcount_read(&ns->refcount));
	}
	mutex_unlock(&shadow_ns_map_lock);

	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			  "members:\n");

	mutex_lock(&shadow_ns_tgid_lock);
	xa_for_each(&shadow_ns_tgid_map, index, tg) {
		struct shadow_ns *cur;

		mutex_lock(&tg->lock);
		cur = tg->cur[type];
		if (cur) {
			pos += scnprintf(buf + pos,
					  pos < buflen ? buflen - pos : 0,
					  "  ns=%u tgid=%d\n",
					  cur->id, tg->tgid);
		}
		if (type == SHADOW_NS_TYPE_PID && tg->pending_pidns) {
			pos += scnprintf(buf + pos,
					  pos < buflen ? buflen - pos : 0,
					  "  ns=%u pending_tgid=%d\n",
					  tg->pending_pidns->id, tg->tgid);
		}
		mutex_unlock(&tg->lock);
	}
	mutex_unlock(&shadow_ns_tgid_lock);

	return pos;
}
EXPORT_SYMBOL_GPL(shadow_ns_diag_snprintf_type);

/*
 * shadow_ns_diag_snprintf() - render the full namespace registry plus which
 * task group (tgid) currently sits in which namespace of each type, into
 * @buf (size @buflen). Returns the number of bytes that would have been
 * written (snprintf() semantics).
 */
size_t shadow_ns_diag_snprintf(char *buf, size_t buflen)
{
	size_t pos = 0;
	unsigned long index;
	struct shadow_ns *ns;
	struct shadow_task_group *tg;

	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			  "namespaces: %d\n", atomic_read(&shadow_ns_count));

	mutex_lock(&shadow_ns_map_lock);
	xa_for_each(&shadow_ns_map, index, ns) {
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				  "  ns id=%u type=%s parent_id=%u refcount=%d\n",
				  ns->id, shadow_ns_diag_type_name(ns->type),
				  ns->parent_id, refcount_read(&ns->refcount));
	}
	mutex_unlock(&shadow_ns_map_lock);

	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			  "task groups (tgid: namespace ids by type currently joined):\n");

	mutex_lock(&shadow_ns_tgid_lock);
	xa_for_each(&shadow_ns_tgid_map, index, tg) {
		u32 type;

		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				  "  tgid=%d:", tg->tgid);

		mutex_lock(&tg->lock);
		for (type = 0; type < SHADOW_NS_TYPE_MAX; type++) {
			if (tg->cur[type])
				pos += scnprintf(buf + pos,
						  pos < buflen ? buflen - pos : 0,
						  " %s=%u", shadow_ns_diag_type_name(type),
						  tg->cur[type]->id);
		}
		if (tg->pending_pidns)
			pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
					  " pending_pid=%u", tg->pending_pidns->id);
		mutex_unlock(&tg->lock);

		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0, "\n");
	}
	mutex_unlock(&shadow_ns_tgid_lock);

	return pos;
}
EXPORT_SYMBOL_GPL(shadow_ns_diag_snprintf);
