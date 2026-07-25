// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_kernel_diag.c - diagnostics renderer for vendor_kernel.
 * This is NEW code (not vendored from kernel-common).
 */
#include <linux/kernel.h>
#include <linux/hashtable.h>

#include "../vendor_kernel.h"
#include "../ipc/util.h"

size_t vendor_kernel_diag_snprintf(char *buf, size_t buflen)
{
	size_t pos = 0;
	struct vns_task *t;
	unsigned int bkt;
	unsigned long flags;

	spin_lock_irqsave(&vendor_kernel_registry.lock, flags);
	pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
			 "enabled: %s\n"
			 "tasks: %lu\n"
			 "stat_unshare: %lu\n"
			 "stat_setns: %lu\n"
			 "stat_clone: %lu\n",
			 vendor_kernel_enabled ? "yes" : "no",
			 vendor_kernel_registry.task_count,
			 vendor_kernel_registry.stat_unshare,
			 vendor_kernel_registry.stat_setns,
			 vendor_kernel_registry.stat_clone);
	hash_for_each(vendor_kernel_registry.tasks, bkt, t, node)
		pos += scnprintf(buf + pos, pos < buflen ? buflen - pos : 0,
				 "tgid=%d nsproxy=%px ipc_ns=%px msg_ids=%d sem_ids=%d shm_ids=%d mq_queues=%u\n",
				 t->tgid, t->nsproxy,
				 t->nsproxy ? t->nsproxy->ipc_ns : NULL,
				 t->nsproxy && t->nsproxy->ipc_ns ?
				 t->nsproxy->ipc_ns->ids[IPC_MSG_IDS].in_use : -1,
				 t->nsproxy && t->nsproxy->ipc_ns ?
				 t->nsproxy->ipc_ns->ids[IPC_SEM_IDS].in_use : -1,
				 t->nsproxy && t->nsproxy->ipc_ns ?
				 t->nsproxy->ipc_ns->ids[IPC_SHM_IDS].in_use : -1,
				 t->nsproxy && t->nsproxy->ipc_ns ?
				 t->nsproxy->ipc_ns->mq_queues_count : 0);
	spin_unlock_irqrestore(&vendor_kernel_registry.lock, flags);
	return pos;
}
