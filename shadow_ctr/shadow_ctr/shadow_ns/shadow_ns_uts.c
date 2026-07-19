// SPDX-License-Identifier: GPL-2.0
#include "shadow_ns_internal.h"

static long (*real_sys_sethostname)(const struct pt_regs *regs);
static long (*real_sys_setdomainname)(const struct pt_regs *regs);
static long (*real_sys_newuname)(const struct pt_regs *regs);

static const char * const shadow_ns_sethostname_names[] = {
	"__arm64_sys_sethostname", "__x64_sys_sethostname", "sys_sethostname", NULL,
};
static const char * const shadow_ns_setdomainname_names[] = {
	"__arm64_sys_setdomainname", "__x64_sys_setdomainname",
	"sys_setdomainname", NULL,
};
static const char * const shadow_ns_newuname_names[] = {
	"__arm64_sys_newuname", "__x64_sys_newuname", "sys_newuname",
	"__arm64_sys_uname", "__x64_sys_uname", "sys_uname", NULL,
};

struct shadow_uts_priv *shadow_ns_uts_priv_alloc(struct shadow_uts_priv *parent)
{
	struct shadow_uts_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	mutex_init(&priv->lock);
	if (parent) {
		mutex_lock(&parent->lock);
		strscpy(priv->nodename, parent->nodename, sizeof(priv->nodename));
		strscpy(priv->domainname, parent->domainname,
			sizeof(priv->domainname));
		mutex_unlock(&parent->lock);
	}
	return priv;
}

void shadow_ns_uts_priv_free(struct shadow_uts_priv *priv)
{
	if (!priv)
		return;
	mutex_destroy(&priv->lock);
	kfree(priv);
}

static long shadow_ns_uts_update(bool domainname, const char __user *name,
				 int len)
{
	struct shadow_ns *ns;
	struct shadow_uts_priv *p;
	char buf[SHADOW_NS_UTS_LEN + 1] = { 0 };

	if (len < 0 || len > SHADOW_NS_UTS_LEN)
		return -EINVAL;

	ns = shadow_ns_get_current(SHADOW_NS_TYPE_UTS);
	if (!ns)
		return -ENOENT;
	p = ns->uts;
	if (!p) {
		shadow_ns_put(ns);
		return -ENOENT;
	}
	if (!capable(CAP_SYS_ADMIN)) {
		shadow_ns_put(ns);
		return -EPERM;
	}
	if (len && copy_from_user(buf, name, len)) {
		shadow_ns_put(ns);
		return -EFAULT;
	}
	buf[len] = '\0';

	mutex_lock(&p->lock);
	if (domainname)
		strscpy(p->domainname, buf, sizeof(p->domainname));
	else
		strscpy(p->nodename, buf, sizeof(p->nodename));
	mutex_unlock(&p->lock);

	shadow_ns_put(ns);
	return 0;
}

static void shadow_ns_uts_capture(char nodename[SHADOW_NS_UTS_LEN + 1],
				  char domainname[SHADOW_NS_UTS_LEN + 1],
				  bool *has)
{
	struct shadow_ns *ns;
	struct shadow_uts_priv *p;

	*has = false;

	ns = shadow_ns_get_current(SHADOW_NS_TYPE_UTS);
	if (!ns)
		return;
	p = ns->uts;
	if (!p) {
		shadow_ns_put(ns);
		return;
	}

	mutex_lock(&p->lock);
	strscpy(nodename, p->nodename, SHADOW_NS_UTS_LEN + 1);
	strscpy(domainname, p->domainname, SHADOW_NS_UTS_LEN + 1);
	mutex_unlock(&p->lock);
	*has = true;

	shadow_ns_put(ns);
}

static long shadow_ns_hook_sethostname(const struct pt_regs *regs)
{
	const char __user *name =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	int len = (int)shadow_ns_sys_arg1(regs);
	long ret = shadow_ns_uts_update(false, name, len);

	if (ret == -ENOENT)
		return real_sys_sethostname(regs);
	return ret;
}

static long shadow_ns_hook_setdomainname(const struct pt_regs *regs)
{
	const char __user *name =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	int len = (int)shadow_ns_sys_arg1(regs);
	long ret = shadow_ns_uts_update(true, name, len);

	if (ret == -ENOENT)
		return real_sys_setdomainname(regs);
	return ret;
}

static long shadow_ns_hook_newuname(const struct pt_regs *regs)
{
	struct new_utsname uts;
	char nodename[SHADOW_NS_UTS_LEN + 1];
	char domainname[SHADOW_NS_UTS_LEN + 1];
	void __user *uarg = (void __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
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

static struct shadow_hook shadow_ns_sethostname_hook =
	SHADOW_HOOK(shadow_ns_sethostname_names, shadow_ns_hook_sethostname,
		    &real_sys_sethostname);
static struct shadow_hook shadow_ns_setdomainname_hook =
	SHADOW_HOOK(shadow_ns_setdomainname_names, shadow_ns_hook_setdomainname,
		    &real_sys_setdomainname);
static struct shadow_hook shadow_ns_newuname_hook =
	SHADOW_HOOK(shadow_ns_newuname_names, shadow_ns_hook_newuname,
		    &real_sys_newuname);

struct shadow_hook *shadow_ns_uts_hooks[] = {
	&shadow_ns_sethostname_hook,
	&shadow_ns_setdomainname_hook,
	&shadow_ns_newuname_hook,
	NULL,
};
