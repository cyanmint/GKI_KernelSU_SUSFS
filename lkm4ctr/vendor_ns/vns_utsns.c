// SPDX-License-Identifier: GPL-2.0
/*
 * vns_utsns.c - vendored UTS namespace.
 *
 * Vendored/adapted from kernel/utsname.c and include/linux/utsname.h (Linux
 * kernel, GPL-2.0). Adapted for out-of-tree module use: symbols renamed under a
 * vns_ prefix, ucounts/user-ns/nsfs plumbing replaced by this module's
 * vns_ns_common bookkeeping. The algorithm is the kernel's clone_uts_ns(): a
 * fresh uts_namespace is allocated and the parent's struct new_utsname payload
 * is copied verbatim (kernel does this under uts_sem; here the whole clone runs
 * under vendor_ns_registry.lock).
 *
 * UTS is the one namespace type this module vendors fully functionally from a
 * loadable module: sethostname(2)/setdomainname(2) store into a live
 * per-namespace vns_new_utsname, and uname(2)/gethostname(2) read it back (see
 * vendor_ns_syscalls.c).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/utsname.h>
#include <linux/string.h>

#include "vendor_ns.h"
#include "../../common/lkm4ctr_log.h"

/*
 * Seed the root vendored UTS namespace from the host's current new_utsname so
 * uname(2) read-back through the vendored path shows the real system identity
 * until a container overrides it.
 */
static void vns_uts_seed_root(struct vns_new_utsname *name)
{
	struct new_utsname *host = utsname();

	memset(name, 0, sizeof(*name));
	strscpy(name->sysname, host->sysname, sizeof(name->sysname));
	strscpy(name->nodename, host->nodename, sizeof(name->nodename));
	strscpy(name->release, host->release, sizeof(name->release));
	strscpy(name->version, host->version, sizeof(name->version));
	strscpy(name->machine, host->machine, sizeof(name->machine));
	strscpy(name->domainname, host->domainname, sizeof(name->domainname));
}

struct vns_uts_namespace *vns_uts_root(void)
{
	struct vns_uts_namespace *ns;

	lockdep_assert_held(&vendor_ns_registry.lock);

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return NULL;

	vns_uts_seed_root(&ns->name);
	vns_ns_common_init(&ns->ns, VENDOR_NS_TYPE_UTS);
	vns_ns_register(&ns->ns);
	return ns;
}

/* Vendored from clone_uts_ns() (kernel/utsname.c). */
struct vns_uts_namespace *vns_clone_uts_ns(struct vns_uts_namespace *old)
{
	struct vns_uts_namespace *ns;

	lockdep_assert_held(&vendor_ns_registry.lock);

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return NULL;

	/* kernel: memcpy(&ns->name, &old_ns->name, sizeof(ns->name)); */
	if (old)
		memcpy(&ns->name, &old->name, sizeof(ns->name));

	vns_ns_common_init(&ns->ns, VENDOR_NS_TYPE_UTS);
	vns_ns_register(&ns->ns);
	return ns;
}

void vns_free_uts_ns(struct vns_uts_namespace *ns)
{
	if (!ns)
		return;

	lockdep_assert_held(&vendor_ns_registry.lock);

	if (!refcount_dec_and_test(&ns->ns.count))
		return;

	vns_ns_unregister(&ns->ns);
	kfree(ns);
}
