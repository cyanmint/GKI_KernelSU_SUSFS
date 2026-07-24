// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_ns_diag.c - diagnostics renderer for vendor_ns.
 * This is NEW code (not vendored from kernel-common).
 */
#include <linux/kernel.h>
#include <linux/hashtable.h>

#include "../vendor_ns.h"

size_t vendor_ns_diag_snprintf(char *buf, size_t buflen)
{
	size_t pos = 0;
	struct vns_task *t;
	unsigned int bkt;
	unsigned long flags;

	spin_lock_irqsave(&vendor_ns_registry.lock, flags);
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "enabled: %s\n"
			 "tasks: %lu\n"
			 "stat_unshare: %lu\n"
			 "stat_setns: %lu\n"
			 "stat_clone: %lu\n",
			 vendor_ns_enabled ? "yes" : "no",
			 vendor_ns_registry.task_count,
			 vendor_ns_registry.stat_unshare,
			 vendor_ns_registry.stat_setns,
			 vendor_ns_registry.stat_clone);
	hash_for_each(vendor_ns_registry.tasks, bkt, t, node)
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "tgid=%d nsproxy=%px\n", t->tgid, t->nsproxy);
	spin_unlock_irqrestore(&vendor_ns_registry.lock, flags);
	return pos;
}
