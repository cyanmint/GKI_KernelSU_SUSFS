// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns_net - network namespace presence/extension module for shadow_ns_base
 *
 * This is a thin, independently loadable module. It does NOT add any new
 * functional isolation: the actual network shadow namespace bookkeeping
 * (allocation, refcounting, and the unshare/setns/clone/fork hooks that create
 * and join namespaces of every type) already lives entirely in
 * shadow_ns_base.ko and works whether or not this module is loaded.
 *
 * What loading this module does:
 *   (a) makes explicit that the deployer has chosen to "enable" the network
 *       (SHADOW_NS_TYPE_NET) shadow namespace type, and
 *   (b) claims the per-type plugin slot for SHADOW_NS_TYPE_NET, providing a ready
 *       extension point for a future implementation that adds genuine per-type
 *       behaviour (a payload via priv_alloc/priv_free and/or custom ioctls)
 *       WITHOUT having to modify shadow_ns_base again.
 *
 * It registers an ops vector with real_support=false and no payload/ioctl
 * callbacks, i.e. exactly today's refcounted-bookkeeping fidelity for this
 * namespace type. Requires shadow_ns_base.ko to be loaded first.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>

#include "shadow_ns_base.h"
#include "include/uapi/shadow_ns.h"

#define SHADOW_NS_NET_VERSION	"2.0"

static const struct shadow_ns_type_ops shadow_ns_net_ops = {
	.owner		= THIS_MODULE,
	.priv_alloc	= NULL,
	.priv_free	= NULL,
	.ioctl		= NULL,
	.real_support	= false,
};

static int __init shadow_ns_net_init(void)
{
	int ret;

	ret = shadow_ns_base_register_type(SHADOW_NS_TYPE_NET, &shadow_ns_net_ops);
	if (ret) {
		pr_err("shadow_ns_net: shadow_ns_base_register_type() failed: %d (is shadow_ns_base loaded?)\n",
		       ret);
		return ret;
	}

	pr_info("shadow_ns_net: registered network namespace type (bookkeeping only, real_support=false)\n");
	return 0;
}

static void __exit shadow_ns_net_exit(void)
{
	shadow_ns_base_unregister_type(SHADOW_NS_TYPE_NET);
	pr_info("shadow_ns_net: unregistered network namespace type\n");
}

module_init(shadow_ns_net_init);
module_exit(shadow_ns_net_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Shadow network namespace presence/extension module (thin plugin for shadow_ns_base.ko; bookkeeping only)");
MODULE_VERSION(SHADOW_NS_NET_VERSION);
