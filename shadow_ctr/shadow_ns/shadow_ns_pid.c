// SPDX-License-Identifier: GPL-2.0
#include "shadow_ns_internal.h"

static long (*real_sys_getpid)(const struct pt_regs *regs);
static long (*real_sys_getppid)(const struct pt_regs *regs);
static long (*real_sys_kill)(const struct pt_regs *regs);
static long (*real_sys_tgkill)(const struct pt_regs *regs);
static long (*real_sys_tkill)(const struct pt_regs *regs);
static long (*real_sys_wait4)(const struct pt_regs *regs);
static long (*real_sys_waitid)(const struct pt_regs *regs);

static const char * const shadow_ns_getpid_names[] = {
	"__arm64_sys_getpid", "__x64_sys_getpid", "sys_getpid", NULL,
};
static const char * const shadow_ns_getppid_names[] = {
	"__arm64_sys_getppid", "__x64_sys_getppid", "sys_getppid", NULL,
};
static const char * const shadow_ns_kill_names[] = {
	"__arm64_sys_kill", "__x64_sys_kill", "sys_kill", NULL,
};
static const char * const shadow_ns_tgkill_names[] = {
	"__arm64_sys_tgkill", "__x64_sys_tgkill", "sys_tgkill", NULL,
};
static const char * const shadow_ns_tkill_names[] = {
	"__arm64_sys_tkill", "__x64_sys_tkill", "sys_tkill", NULL,
};
static const char * const shadow_ns_wait4_names[] = {
	"__arm64_sys_wait4", "__x64_sys_wait4", "sys_wait4", NULL,
};
static const char * const shadow_ns_waitid_names[] = {
	"__arm64_sys_waitid", "__x64_sys_waitid", "sys_waitid", NULL,
};

struct shadow_pidns_priv *shadow_ns_pidns_priv_alloc(void)
{
	struct shadow_pidns_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	mutex_init(&priv->lock);
	xa_init_flags(&priv->vpid_to_rpid, XA_FLAGS_ALLOC);
	xa_init(&priv->rpid_to_vpid);
	priv->next_vpid = 1;
	return priv;
}

void shadow_ns_pidns_priv_free(struct shadow_pidns_priv *priv)
{
	if (!priv)
		return;
	xa_destroy(&priv->vpid_to_rpid);
	xa_destroy(&priv->rpid_to_vpid);
	mutex_destroy(&priv->lock);
	kfree(priv);
}

void shadow_ns_pidns_register(struct shadow_pidns_priv *pidns, pid_t rpid)
{
	u32 vpid;

	if (!pidns || rpid <= 0)
		return;

	mutex_lock(&pidns->lock);
	if (xa_load(&pidns->rpid_to_vpid, rpid)) {
		mutex_unlock(&pidns->lock);
		return;
	}
	vpid = pidns->next_vpid++;
	if (xa_err(xa_store(&pidns->vpid_to_rpid, vpid, xa_mk_value(rpid), GFP_KERNEL)))
		goto unlock;
	if (xa_err(xa_store(&pidns->rpid_to_vpid, rpid, xa_mk_value(vpid), GFP_KERNEL)))
		xa_erase(&pidns->vpid_to_rpid, vpid);
unlock:
	mutex_unlock(&pidns->lock);
}

void shadow_ns_pidns_unregister(struct shadow_pidns_priv *pidns, pid_t rpid)
{
	void *v;

	if (!pidns || rpid <= 0)
		return;

	mutex_lock(&pidns->lock);
	v = xa_erase(&pidns->rpid_to_vpid, rpid);
	if (v)
		xa_erase(&pidns->vpid_to_rpid, xa_to_value(v));
	mutex_unlock(&pidns->lock);
}

pid_t shadow_ns_pidns_to_vpid(struct shadow_pidns_priv *pidns, pid_t rpid)
{
	void *v;
	pid_t vpid = 0;

	if (!pidns || rpid <= 0)
		return 0;
	mutex_lock(&pidns->lock);
	v = xa_load(&pidns->rpid_to_vpid, rpid);
	if (v)
		vpid = (pid_t)xa_to_value(v);
	mutex_unlock(&pidns->lock);
	return vpid;
}

pid_t shadow_ns_pidns_to_rpid(struct shadow_pidns_priv *pidns, pid_t vpid)
{
	void *v;
	pid_t rpid = 0;

	if (!pidns || vpid <= 0)
		return 0;
	mutex_lock(&pidns->lock);
	v = xa_load(&pidns->vpid_to_rpid, vpid);
	if (v)
		rpid = (pid_t)xa_to_value(v);
	mutex_unlock(&pidns->lock);
	return rpid;
}

struct shadow_ns *shadow_ns_current_pidns(void)
{
	return shadow_ns_get_current(SHADOW_NS_TYPE_PID);
}

static long shadow_ns_hook_getpid(const struct pt_regs *regs)
{
	struct shadow_ns *ns = shadow_ns_current_pidns();
	pid_t rpid = task_tgid_nr(current);
	pid_t vpid;

	if (!ns)
		return real_sys_getpid(regs);

	vpid = shadow_ns_pidns_to_vpid(ns->pid, rpid);
	shadow_ns_put(ns);
	return vpid ? vpid : real_sys_getpid(regs);
}

static long shadow_ns_hook_getppid(const struct pt_regs *regs)
{
	struct shadow_ns *ns = shadow_ns_current_pidns();
	long real_ppid;
	pid_t vppid;

	if (!ns)
		return real_sys_getppid(regs);

	real_ppid = real_sys_getppid(regs);
	if (real_ppid <= 0) {
		shadow_ns_put(ns);
		return real_ppid;
	}
	vppid = shadow_ns_pidns_to_vpid(ns->pid, (pid_t)real_ppid);
	shadow_ns_put(ns);
	return vppid;
}

static long shadow_ns_pid_translate_target(unsigned long vpid_arg)
{
	struct shadow_ns *ns;
	pid_t rpid;

	if ((long)vpid_arg <= 0)
		return vpid_arg;

	ns = shadow_ns_current_pidns();
	if (!ns)
		return vpid_arg;

	rpid = shadow_ns_pidns_to_rpid(ns->pid, (pid_t)vpid_arg);
	shadow_ns_put(ns);
	return rpid ? rpid : vpid_arg;
}

static long shadow_ns_hook_kill(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	return real_sys_kill(&regs_copy);
}

static long shadow_ns_hook_tgkill(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long tgid_arg = shadow_ns_sys_arg0(regs);
	unsigned long tid_arg = shadow_ns_sys_arg1(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(tgid_arg));
	shadow_ns_sys_set_arg1(&regs_copy, shadow_ns_pid_translate_target(tid_arg));
	return real_sys_tgkill(&regs_copy);
}

static long shadow_ns_hook_tkill(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	return real_sys_tkill(&regs_copy);
}

static long shadow_ns_hook_wait4(const struct pt_regs *regs)
{
	struct shadow_ns *ns = shadow_ns_current_pidns();
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);
	long ret;
	pid_t vpid;

	if (!ns)
		return real_sys_wait4(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	ret = real_sys_wait4(&regs_copy);
	if (ret > 0) {
		vpid = shadow_ns_pidns_to_vpid(ns->pid, (pid_t)ret);
		if (vpid)
			ret = vpid;
	}
	shadow_ns_put(ns);
	return ret;
}

static long shadow_ns_hook_waitid(const struct pt_regs *regs)
{
	int idtype = (int)shadow_ns_sys_arg0(regs);
	unsigned long id_arg = shadow_ns_sys_arg1(regs);
	struct pt_regs regs_copy = *regs;

	if (idtype == 1)
		shadow_ns_sys_set_arg1(&regs_copy, shadow_ns_pid_translate_target(id_arg));
	return real_sys_waitid(&regs_copy);
}

static struct shadow_hook shadow_ns_getpid_hook =
	SHADOW_HOOK(shadow_ns_getpid_names, shadow_ns_hook_getpid, &real_sys_getpid);
static struct shadow_hook shadow_ns_getppid_hook =
	SHADOW_HOOK(shadow_ns_getppid_names, shadow_ns_hook_getppid, &real_sys_getppid);
static struct shadow_hook shadow_ns_kill_hook =
	SHADOW_HOOK(shadow_ns_kill_names, shadow_ns_hook_kill, &real_sys_kill);
static struct shadow_hook shadow_ns_tgkill_hook =
	SHADOW_HOOK(shadow_ns_tgkill_names, shadow_ns_hook_tgkill, &real_sys_tgkill);
static struct shadow_hook shadow_ns_tkill_hook =
	SHADOW_HOOK(shadow_ns_tkill_names, shadow_ns_hook_tkill, &real_sys_tkill);
static struct shadow_hook shadow_ns_wait4_hook =
	SHADOW_HOOK(shadow_ns_wait4_names, shadow_ns_hook_wait4, &real_sys_wait4);
static struct shadow_hook shadow_ns_waitid_hook =
	SHADOW_HOOK(shadow_ns_waitid_names, shadow_ns_hook_waitid, &real_sys_waitid);

struct shadow_hook *shadow_ns_pid_hooks[] = {
	&shadow_ns_getpid_hook,
	&shadow_ns_getppid_hook,
	&shadow_ns_kill_hook,
	&shadow_ns_tgkill_hook,
	&shadow_ns_tkill_hook,
	&shadow_ns_wait4_hook,
	&shadow_ns_waitid_hook,
	NULL,
};
