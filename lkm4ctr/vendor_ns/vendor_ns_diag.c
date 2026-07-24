// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_ns_diag.c - diagfs rendering for vendor_ns.
 *
 * vendor_ns's own plumbing (not vendored kernel source). Renders the aggregate
 * and per-namespace-type introspection text consumed by lkm4ctr_diagfs.c's
 * ./mnt/vendor_ns/ tree (status/namespaces/references and the per-type
 * sub-directories). Purely reflects the vendored registry state; no locks are
 * taken beyond vendor_ns_registry.lock while snapshotting.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#include "vendor_ns.h"
#include "../../common/lkm4ctr_log.h"

static const char *vns_type_behaviour(u32 type)
{
	switch (type) {
	case VENDOR_NS_TYPE_UTS:	return "functional (nodename/domainname)";
	case VENDOR_NS_TYPE_PID:	return "functional (idr numbering)";
	case VENDOR_NS_TYPE_USER:	return "functional (uid/gid extent maps)";
	case VENDOR_NS_TYPE_IPC:	return "bookkeeping";
	case VENDOR_NS_TYPE_MNT:	return "bookkeeping";
	case VENDOR_NS_TYPE_CGROUP:	return "bookkeeping";
	case VENDOR_NS_TYPE_NET:	return "bookkeeping";
	case VENDOR_NS_TYPE_TIME:	return "bookkeeping";
	default:			return "?";
	}
}

size_t vendor_ns_diag_snprintf(char *buf, size_t buflen)
{
	size_t pos = 0;
	u32 type;

	if (!buf || !buflen)
		return 0;

	mutex_lock(&vendor_ns_registry.lock);

	pos += scnprintf(buf + pos, buflen - pos,
			 "vendor_ns %s (unconditional, fully-vendored namespace subsystem)\n",
			 VENDOR_NS_VERSION);
	pos += scnprintf(buf + pos, buflen - pos,
			 "tracked thread groups: %lu\n",
			 vendor_ns_registry.task_count);
	pos += scnprintf(buf + pos, buflen - pos,
			 "syscalls: unshare=%lu setns=%lu clone=%lu uts_set=%lu uts_get=%lu pid_xlate=%lu id_xlate=%lu proc_filtered=%lu\n",
			 vendor_ns_registry.stat_unshare,
			 vendor_ns_registry.stat_setns,
			 vendor_ns_registry.stat_clone,
			 vendor_ns_registry.stat_uts_set,
			 vendor_ns_registry.stat_uts_get,
			 vendor_ns_registry.stat_pid_xlate,
			 vendor_ns_registry.stat_uid_xlate,
			 vendor_ns_registry.stat_proc_filtered);

	pos += scnprintf(buf + pos, buflen - pos, "namespaces by type:\n");
	for (type = 0; type < VENDOR_NS_TYPE_MAX; type++) {
		pos += scnprintf(buf + pos, buflen - pos,
				 "  %-6s live=%lu created=%lu  [%s]\n",
				 vns_type_name(type),
				 vendor_ns_registry.ns_count[type],
				 vendor_ns_registry.ns_created[type],
				 vns_type_behaviour(type));
	}

	mutex_unlock(&vendor_ns_registry.lock);
	return pos;
}

size_t vendor_ns_diag_snprintf_type(u32 type, char *buf, size_t buflen)
{
	size_t pos = 0;
	struct vns_ns_common *ns;

	if (!buf || !buflen)
		return 0;
	if (type >= VENDOR_NS_TYPE_MAX)
		return scnprintf(buf, buflen, "invalid namespace type %u\n", type);

	mutex_lock(&vendor_ns_registry.lock);

	pos += scnprintf(buf + pos, buflen - pos,
			 "vendor_ns %s namespace type: behaviour=%s live=%lu created=%lu\n",
			 vns_type_name(type), vns_type_behaviour(type),
			 vendor_ns_registry.ns_count[type],
			 vendor_ns_registry.ns_created[type]);

	list_for_each_entry(ns, &vendor_ns_registry.ns_list[type], registry) {
		pos += scnprintf(buf + pos, buflen - pos,
				 "  inum=%u refs=%u\n",
				 ns->inum, refcount_read(&ns->count));

		if (type == VENDOR_NS_TYPE_UTS) {
			struct vns_uts_namespace *uts =
				container_of(ns, struct vns_uts_namespace, ns);
			pos += scnprintf(buf + pos, buflen - pos,
					 "    nodename=\"%s\" domainname=\"%s\"\n",
					 uts->name.nodename,
					 uts->name.domainname);
		} else if (type == VENDOR_NS_TYPE_USER) {
			struct vns_user_namespace *u =
				container_of(ns, struct vns_user_namespace, ns);
			pos += scnprintf(buf + pos, buflen - pos,
					 "    level=%d uid_extents=%u gid_extents=%u\n",
					 u->level, u->uid_map.nr_extents,
					 u->gid_map.nr_extents);
		} else if (type == VENDOR_NS_TYPE_PID) {
			struct vns_pid_namespace *p =
				container_of(ns, struct vns_pid_namespace, ns);
			pos += scnprintf(buf + pos, buflen - pos,
					 "    level=%d pid_max=%d adding=%d cursor=%u\n",
					 p->level, vns_get_pid_max(),
					 !!(p->pid_allocated & VNS_PIDNS_ADDING),
					 idr_get_cursor(&p->idr));
		}
	}

	mutex_unlock(&vendor_ns_registry.lock);
	return pos;
}
