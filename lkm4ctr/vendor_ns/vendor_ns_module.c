// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_ns_module.c - vendor_ns lifecycle (load/unload) and hook management.
 *
 * vendor_ns's own plumbing (not vendored kernel source). Owns the single global
 * vendored-namespace registry, builds the root vendored namespaces for every
 * type at load, installs the unconditional syscall hooks via the shared
 * shadow_hook engine, and tears everything down at unload.
 *
 * Unlike shadow_ns, no CONFIG_*_NS gating is performed: vendor_ns installs
 * every hook group unconditionally. vendor_ns and shadow_ns are mutually
 * exclusive at runtime (enforced by lkm4ctr_diagfs.c's load path) because both
 * target the same syscall symbols and the shared hook engine permits only one
 * hook per symbol.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>

#include "vendor_ns.h"
#include "../../common/shadow_hook.h"
#include "../../common/lkm4ctr_log.h"

struct vns_registry vendor_ns_registry;

static bool vendor_ns_loaded;

/* ---- root namespace construction / teardown (registry lock held) ---- */

static int vns_build_roots(void)
{
	u32 type;

	lockdep_assert_held(&vendor_ns_registry.lock);

	vendor_ns_registry.root_user = vns_user_root();
	if (!vendor_ns_registry.root_user)
		return -ENOMEM;

	vendor_ns_registry.root_uts = vns_uts_root();
	if (!vendor_ns_registry.root_uts)
		return -ENOMEM;

	vendor_ns_registry.root_pid = vns_pid_root();
	if (!vendor_ns_registry.root_pid)
		return -ENOMEM;

	for (type = 0; type < VENDOR_NS_TYPE_MAX; type++) {
		switch (type) {
		case VENDOR_NS_TYPE_IPC:
		case VENDOR_NS_TYPE_MNT:
		case VENDOR_NS_TYPE_NET:
		case VENDOR_NS_TYPE_CGROUP:
		case VENDOR_NS_TYPE_TIME:
			vendor_ns_registry.root_generic[type] =
				vns_generic_root(type);
			if (!vendor_ns_registry.root_generic[type])
				return -ENOMEM;
			break;
		default:
			break;
		}
	}

	vendor_ns_registry.root_nsproxy = vns_nsproxy_root();
	if (!vendor_ns_registry.root_nsproxy)
		return -ENOMEM;

	return 0;
}

static void vns_free_roots(void)
{
	u32 type;

	lockdep_assert_held(&vendor_ns_registry.lock);

	if (vendor_ns_registry.root_nsproxy) {
		if (refcount_dec_and_test(&vendor_ns_registry.root_nsproxy->count))
			kfree(vendor_ns_registry.root_nsproxy);
		vendor_ns_registry.root_nsproxy = NULL;
	}

	vns_free_uts_ns(vendor_ns_registry.root_uts);
	vendor_ns_registry.root_uts = NULL;
	vns_free_user_ns(vendor_ns_registry.root_user);
	vendor_ns_registry.root_user = NULL;
	vns_free_pid_ns(vendor_ns_registry.root_pid);
	vendor_ns_registry.root_pid = NULL;

	for (type = 0; type < VENDOR_NS_TYPE_MAX; type++) {
		vns_free_generic_ns(vendor_ns_registry.root_generic[type]);
		vendor_ns_registry.root_generic[type] = NULL;
	}
}

/* ---- hook installation ---- */

struct vns_hook_group {
	struct shadow_hook **hooks;
	const char *tag;
};

static const struct vns_hook_group vns_hook_groups[] = {
	{ vendor_ns_core_hooks,		"vendor_ns" },
	{ vendor_ns_uts_hooks,		"vendor_ns_uts" },
	{ vendor_ns_pid_hooks,		"vendor_ns_pid" },
	{ vendor_ns_user_hooks,		"vendor_ns_user" },
	{ vendor_ns_procfs_hooks,	"vendor_ns_procfs" },
};

static void vns_remove_all_hooks(void)
{
	int i;

	for (i = ARRAY_SIZE(vns_hook_groups) - 1; i >= 0; i--)
		shadow_hook_remove_all(vns_hook_groups[i].hooks);
}

static int vns_install_all_hooks(void)
{
	int i, hooked, total = 0;

	for (i = 0; i < ARRAY_SIZE(vns_hook_groups); i++) {
		hooked = shadow_hook_install_all(vns_hook_groups[i].hooks,
						 vns_hook_groups[i].tag);
		if (hooked < 0) {
			LKM4CTR_ERR(VENDOR_NS_TAG,
				    "init: shadow_hook_install_all(%s) failed: %d",
				    vns_hook_groups[i].tag, hooked);
			vns_remove_all_hooks();
			return hooked;
		}
		LKM4CTR_INFO(VENDOR_NS_TAG,
			     "init: %d hook(s) installed for group '%s'",
			     hooked, vns_hook_groups[i].tag);
		total += hooked;
	}
	return total;
}

/* ---- lifecycle entry points (called from lkm4ctr_diagfs.c load path) ---- */

int vendor_ns_init(void)
{
	u32 type;
	int ret;

	if (vendor_ns_loaded)
		return 0;

	LKM4CTR_INFO(VENDOR_NS_TAG, "init starting (version %s)",
		     VENDOR_NS_VERSION);

	mutex_init(&vendor_ns_registry.lock);
	hash_init(vendor_ns_registry.tasks);
	vendor_ns_registry.next_inum = 0;
	vendor_ns_registry.task_count = 0;
	for (type = 0; type < VENDOR_NS_TYPE_MAX; type++) {
		INIT_LIST_HEAD(&vendor_ns_registry.ns_list[type]);
		vendor_ns_registry.ns_count[type] = 0;
		vendor_ns_registry.ns_created[type] = 0;
	}

	mutex_lock(&vendor_ns_registry.lock);
	ret = vns_build_roots();
	if (ret) {
		vns_free_roots();
		mutex_unlock(&vendor_ns_registry.lock);
		LKM4CTR_ERR(VENDOR_NS_TAG, "init: root namespace setup failed: %d",
			    ret);
		return ret;
	}
	mutex_unlock(&vendor_ns_registry.lock);

	ret = vns_install_all_hooks();
	if (ret < 0) {
		mutex_lock(&vendor_ns_registry.lock);
		vns_free_roots();
		mutex_unlock(&vendor_ns_registry.lock);
		return ret;
	}

	LKM4CTR_INFO(VENDOR_NS_TAG,
		     "init complete: %d total hook(s), all namespace types vendored unconditionally",
		     ret);
	vendor_ns_loaded = true;
	return 0;
}

void vendor_ns_exit(void)
{
	if (!vendor_ns_loaded)
		return;

	LKM4CTR_INFO(VENDOR_NS_TAG, "exit: removing hooks and tearing down registry");

	vns_remove_all_hooks();
	vns_task_purge_all();

	mutex_lock(&vendor_ns_registry.lock);
	vns_free_roots();
	mutex_unlock(&vendor_ns_registry.lock);

	vendor_ns_loaded = false;
}
