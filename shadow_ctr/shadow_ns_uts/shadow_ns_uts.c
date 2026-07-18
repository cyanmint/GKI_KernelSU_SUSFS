// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns_uts - UTS (nodename/domainname) extension for shadow_ns_base
 *
 * shadow_ns_base.ko provides the generic shadow-namespace registry and the
 * unshare/setns/clone/fork hooks that create/join shadow namespaces of every
 * type as reference-counted bookkeeping. This module adds the one namespace
 * type that gets *genuine* functional behaviour: UTS, i.e. real per-namespace
 * nodename/domainname storage.
 *
 * It plugs into shadow_ns_base via the plugin API (common/shadow_ns_base.h):
 *   - a per-UTS-namespace payload (struct shadow_uts_priv) holding the
 *     nodename/domainname, allocated/freed through priv_alloc/priv_free; and
 *   - transparent hooks (installed by *this* module, via shadow_hook.h) on the
 *     sethostname/setdomainname/newuname(uname) syscalls, so an unmodified
 *     process observing/altering its hostname sees its shadow UTS namespace.
 *
 * shadow_ns_base.ko must be loaded first; this module resolves its exported
 * symbols at load time (its Makefile wires KBUILD_EXTRA_SYMBOLS to
 * shadow_ns_base's Module.symvers so modpost resolves them at build time).
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/capability.h>
#include <linux/utsname.h>
#include <linux/err.h>
#include <asm/ptrace.h>

#include "shadow_hook.h"
#include "shadow_ns_base.h"
#include "include/uapi/shadow_ns.h"

#define SHADOW_NS_UTS_VERSION	"2.0"

/*
 * struct shadow_uts_priv - per-UTS-namespace payload attached to a shadow
 * namespace object via shadow_ns_base's type_priv mechanism.
 * @lock:       serialises reads/writes of the strings below. Owned entirely
 *              by this module, decoupled from shadow_ns_base's internal locks.
 * @nodename:   the shadow hostname (uname -n / gethostname)
 * @domainname: the shadow NIS/domain name (getdomainname)
 */
struct shadow_uts_priv {
	struct mutex	lock;
	char		nodename[SHADOW_NS_UTS_LEN + 1];
	char		domainname[SHADOW_NS_UTS_LEN + 1];
};

/* --- plugin payload callbacks ------------------------------------------- */

static void *shadow_ns_uts_priv_alloc(u32 parent_id, void *parent_priv)
{
	struct shadow_uts_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	mutex_init(&priv->lock);

	/* Inherit the parent namespace's nodename/domainname, if any. */
	if (parent_priv) {
		struct shadow_uts_priv *parent = parent_priv;

		mutex_lock(&parent->lock);
		strscpy(priv->nodename, parent->nodename, sizeof(priv->nodename));
		strscpy(priv->domainname, parent->domainname,
			sizeof(priv->domainname));
		mutex_unlock(&parent->lock);
	}

	return priv;
}

static void shadow_ns_uts_priv_free(void *priv)
{
	struct shadow_uts_priv *p = priv;

	if (!p)
		return;
	mutex_destroy(&p->lock);
	kfree(p);
}

static const struct shadow_ns_type_ops shadow_ns_uts_ops = {
	.owner		= THIS_MODULE,
	.priv_alloc	= shadow_ns_uts_priv_alloc,
	.priv_free	= shadow_ns_uts_priv_free,
	.real_support	= true,
};

/* --- transparent syscall hooks ------------------------------------------ */

static long (*real_sys_sethostname)(const struct pt_regs *regs);
static long (*real_sys_setdomainname)(const struct pt_regs *regs);
static long (*real_sys_newuname)(const struct pt_regs *regs);

static const char * const shadow_ns_uts_sethostname_names[] = {
	"__arm64_sys_sethostname",
	"__x64_sys_sethostname",
	"sys_sethostname",
	NULL,
};
static const char * const shadow_ns_uts_setdomainname_names[] = {
	"__arm64_sys_setdomainname",
	"__x64_sys_setdomainname",
	"sys_setdomainname",
	NULL,
};
static const char * const shadow_ns_uts_newuname_names[] = {
	"__arm64_sys_newuname",
	"__x64_sys_newuname",
	"sys_newuname",
	"__arm64_sys_uname",
	"__x64_sys_uname",
	"sys_uname",
	NULL,
};

#if defined(CONFIG_ARM64)
static unsigned long shadow_ns_uts_sys_arg0(const struct pt_regs *regs)
{
	return regs->regs[0];
}

static unsigned long shadow_ns_uts_sys_arg1(const struct pt_regs *regs)
{
	return regs->regs[1];
}
#elif defined(CONFIG_X86_64)
static unsigned long shadow_ns_uts_sys_arg0(const struct pt_regs *regs)
{
	return regs->di;
}

static unsigned long shadow_ns_uts_sys_arg1(const struct pt_regs *regs)
{
	return regs->si;
}
#else
#error "shadow_ns_uts: unsupported architecture"
#endif

/*
 * Update the current task group's shadow UTS namespace nodename/domainname.
 * Returns -ENOENT when there is no shadow UTS namespace (or it has no
 * payload), so the caller can fall through to the real syscall.
 */
static long shadow_ns_uts_update(bool domainname, const char __user *name,
				int len)
{
	struct shadow_ns *ns;
	struct shadow_uts_priv *p;
	char buf[SHADOW_NS_UTS_LEN + 1] = { 0 };

	if (len < 0 || len > SHADOW_NS_UTS_LEN)
		return -EINVAL;

	ns = shadow_ns_base_get_current(SHADOW_NS_TYPE_UTS);
	if (!ns)
		return -ENOENT;
	p = shadow_ns_base_priv(ns);
	if (!p) {
		shadow_ns_base_put(ns);
		return -ENOENT;
	}
	if (!capable(CAP_SYS_ADMIN)) {
		shadow_ns_base_put(ns);
		return -EPERM;
	}
	if (len && copy_from_user(buf, name, len)) {
		shadow_ns_base_put(ns);
		return -EFAULT;
	}
	buf[len] = '\0';

	mutex_lock(&p->lock);
	if (domainname)
		strscpy(p->domainname, buf, sizeof(p->domainname));
	else
		strscpy(p->nodename, buf, sizeof(p->nodename));
	mutex_unlock(&p->lock);

	shadow_ns_base_put(ns);
	return 0;
}

/*
 * Capture the current task group's shadow UTS strings into @nodename/
 * @domainname. Sets *@has to true only when a shadow UTS namespace with a
 * payload exists.
 */
static void shadow_ns_uts_capture(char nodename[SHADOW_NS_UTS_LEN + 1],
				 char domainname[SHADOW_NS_UTS_LEN + 1],
				 bool *has)
{
	struct shadow_ns *ns;
	struct shadow_uts_priv *p;

	*has = false;

	ns = shadow_ns_base_get_current(SHADOW_NS_TYPE_UTS);
	if (!ns)
		return;
	p = shadow_ns_base_priv(ns);
	if (!p) {
		shadow_ns_base_put(ns);
		return;
	}

	mutex_lock(&p->lock);
	strscpy(nodename, p->nodename, SHADOW_NS_UTS_LEN + 1);
	strscpy(domainname, p->domainname, SHADOW_NS_UTS_LEN + 1);
	mutex_unlock(&p->lock);
	*has = true;

	shadow_ns_base_put(ns);
}

static long shadow_ns_uts_hook_sethostname(const struct pt_regs *regs)
{
	const char __user *name =
		(const char __user *)(uintptr_t)shadow_ns_uts_sys_arg0(regs);
	int len = (int)shadow_ns_uts_sys_arg1(regs);
	long ret = shadow_ns_uts_update(false, name, len);

	if (ret == -ENOENT)
		return real_sys_sethostname(regs);
	return ret;
}

static long shadow_ns_uts_hook_setdomainname(const struct pt_regs *regs)
{
	const char __user *name =
		(const char __user *)(uintptr_t)shadow_ns_uts_sys_arg0(regs);
	int len = (int)shadow_ns_uts_sys_arg1(regs);
	long ret = shadow_ns_uts_update(true, name, len);

	if (ret == -ENOENT)
		return real_sys_setdomainname(regs);
	return ret;
}

static long shadow_ns_uts_hook_newuname(const struct pt_regs *regs)
{
	struct new_utsname uts;
	char nodename[SHADOW_NS_UTS_LEN + 1];
	char domainname[SHADOW_NS_UTS_LEN + 1];
	void __user *uarg =
		(void __user *)(uintptr_t)shadow_ns_uts_sys_arg0(regs);
	bool has_shadow_uts;
	long ret;

	shadow_ns_uts_capture(nodename, domainname, &has_shadow_uts);
	ret = real_sys_newuname(regs);
	if (ret || !has_shadow_uts)
		return ret;

	if (copy_from_user(&uts, uarg, sizeof(uts)))
		return -EFAULT;
	strscpy(uts.nodename, nodename, sizeof(uts.nodename));
	strscpy(uts.domainname, domainname, sizeof(uts.domainname));
	if (copy_to_user(uarg, &uts, sizeof(uts)))
		return -EFAULT;
	return 0;
}

static struct shadow_hook shadow_ns_uts_sethostname_hook =
	SHADOW_HOOK(shadow_ns_uts_sethostname_names,
		    shadow_ns_uts_hook_sethostname, &real_sys_sethostname);
static struct shadow_hook shadow_ns_uts_setdomainname_hook =
	SHADOW_HOOK(shadow_ns_uts_setdomainname_names,
		    shadow_ns_uts_hook_setdomainname, &real_sys_setdomainname);
static struct shadow_hook shadow_ns_uts_newuname_hook =
	SHADOW_HOOK(shadow_ns_uts_newuname_names,
		    shadow_ns_uts_hook_newuname, &real_sys_newuname);

static struct shadow_hook *shadow_ns_uts_hooks[] = {
	&shadow_ns_uts_sethostname_hook,
	&shadow_ns_uts_setdomainname_hook,
	&shadow_ns_uts_newuname_hook,
	NULL,
};

static int __init shadow_ns_uts_init(void)
{
	int ret;
	int hooked;

	pr_info("shadow_ns_uts: init starting (version %s)\n",
		SHADOW_NS_UTS_VERSION);

	ret = shadow_ns_base_register_type(SHADOW_NS_TYPE_UTS,
					   &shadow_ns_uts_ops);
	if (ret) {
		pr_err("shadow_ns_uts: shadow_ns_base_register_type() failed: %d (is shadow_ns_base loaded?)\n",
		       ret);
		return ret;
	}
	pr_info("shadow_ns_uts: registered UTS extension with shadow_ns_base\n");

	pr_info("shadow_ns_uts: installing sethostname/setdomainname/uname hooks\n");
	hooked = shadow_hook_install_all(shadow_ns_uts_hooks, "shadow_ns_uts");
	if (hooked < 0) {
		ret = hooked;
		pr_err("shadow_ns_uts: shadow_hook_install_all() failed: %d\n", ret);
		shadow_hook_remove_all(shadow_ns_uts_hooks);
		shadow_ns_base_unregister_type(SHADOW_NS_TYPE_UTS);
		return ret;
	}

	pr_info("shadow_ns_uts: loaded (real UTS support, %d hook(s) installed)\n",
		hooked);
	return 0;
}

static void __exit shadow_ns_uts_exit(void)
{
	pr_info("shadow_ns_uts: exit: removing hooks\n");
	shadow_hook_remove_all(shadow_ns_uts_hooks);
	pr_info("shadow_ns_uts: exit: unregistering UTS extension\n");
	shadow_ns_base_unregister_type(SHADOW_NS_TYPE_UTS);
	pr_info("shadow_ns_uts: unloaded\n");
}

module_init(shadow_ns_uts_init);
module_exit(shadow_ns_uts_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Shadow namespace UTS extension: real per-namespace nodename/domainname storage + sethostname/setdomainname/uname hooks (depends on shadow_ns_base.ko)");
MODULE_VERSION(SHADOW_NS_UTS_VERSION);
