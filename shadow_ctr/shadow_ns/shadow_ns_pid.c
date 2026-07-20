// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns_pid - simulated PID namespace fallback, vendored from real
 * kernel/pid.c and kernel/pid_namespace.c algorithms.
 *
 * This file only ever runs when the *booted* kernel genuinely lacks
 * CONFIG_PID_NS (shadow_ns_clone_flags & CLONE_NEWPID -- see
 * shadow_ns_module.c); a kernel with real PID namespace support keeps
 * using its own, fully-conformant kernel/pid_namespace.c and none of this
 * module's code path is reached at all. Because this module cannot touch
 * struct task_struct/struct pid/struct nsproxy themselves (they are not
 * ours to extend from an out-of-tree .ko), the goal here is not to
 * re-implement task_active_pid_ns()/copy_process()'s pid allocation in
 * place, but to reproduce the *same numbering and lifecycle algorithms*
 * kernel/pid.c and kernel/pid_namespace.c use, against our own bookkeeping
 * tables, and to make every user-visible syscall that consumes or produces
 * a pid number (in the previous revision: only getpid/getppid/kill/tgkill/
 * tkill/wait4/waitid) go through the same translation.
 *
 * Vendored algorithms (see the citations on each function below):
 *   - kernel/pid.c:alloc_pid()'s idr_alloc_cyclic() cyclic pid allocation,
 *     including its RESERVED_PIDS wraparound policy.
 *   - kernel/pid_namespace.c:zap_pid_ns_processes()'s "namespace init exits
 *     -> SIGKILL everyone else in the namespace, then stop admitting new
 *     members" cascade (disable_pid_allocation()).
 *
 * Deliberately NOT vendored (out of safe reach for a hook-based module):
 * zap_pid_ns_processes()'s orphan reparenting/reaping via
 * forget_original_parent()+kernel_wait4() (this module doesn't own real
 * parent/child task_struct links, so orphans keep being reaped by the
 * host's genuine, real parent chain -- functionally fine, since that
 * machinery already works without our help), and true multi-level pid
 * numbering across >1 nested shadow PID namespace (each shadow_ns still
 * only tracks one level of vpid<->rpid translation; nesting drops to the
 * innermost active namespace, same documented limitation as before).
 */
#include "shadow_ns_internal.h"

/*
 * SHADOW_NS_RESERVED_PIDS - kernel/pid.c's RESERVED_PIDS (300, not exported
 * via any header -- it is a private #define in that file). alloc_pid()
 * uses it purely as the point where cyclic allocation wraps back to instead
 * of continuing to climb toward pid_max, so init/child-reaper-adjacent low
 * numbers stay available after a namespace has cycled through many pids.
 * Duplicated here verbatim for the same reason, scoped to our own idr.
 */
#define SHADOW_NS_RESERVED_PIDS	300

static long (*real_sys_getpid)(const struct pt_regs *regs);
static long (*real_sys_getppid)(const struct pt_regs *regs);
static long (*real_sys_kill)(const struct pt_regs *regs);
static long (*real_sys_tgkill)(const struct pt_regs *regs);
static long (*real_sys_tkill)(const struct pt_regs *regs);
static long (*real_sys_wait4)(const struct pt_regs *regs);
static long (*real_sys_waitid)(const struct pt_regs *regs);
static long (*real_sys_exit_group)(const struct pt_regs *regs);
static long (*real_sys_setpgid)(const struct pt_regs *regs);
static long (*real_sys_getpgid)(const struct pt_regs *regs);
static long (*real_sys_getsid)(const struct pt_regs *regs);
static long (*real_sys_ptrace)(const struct pt_regs *regs);
static long (*real_sys_rt_sigqueueinfo)(const struct pt_regs *regs);
static long (*real_sys_rt_tgsigqueueinfo)(const struct pt_regs *regs);
static long (*real_sys_pidfd_open)(const struct pt_regs *regs);

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
static const char * const shadow_ns_exit_group_names[] = {
	"__arm64_sys_exit_group", "__x64_sys_exit_group", "sys_exit_group",
	NULL,
};
static const char * const shadow_ns_setpgid_names[] = {
	"__arm64_sys_setpgid", "__x64_sys_setpgid", "sys_setpgid", NULL,
};
static const char * const shadow_ns_getpgid_names[] = {
	"__arm64_sys_getpgid", "__x64_sys_getpgid", "sys_getpgid", NULL,
};
static const char * const shadow_ns_getsid_names[] = {
	"__arm64_sys_getsid", "__x64_sys_getsid", "sys_getsid", NULL,
};
static const char * const shadow_ns_ptrace_names[] = {
	"__arm64_sys_ptrace", "__x64_sys_ptrace", "sys_ptrace", NULL,
};
static const char * const shadow_ns_rt_sigqueueinfo_names[] = {
	"__arm64_sys_rt_sigqueueinfo", "__x64_sys_rt_sigqueueinfo",
	"sys_rt_sigqueueinfo", NULL,
};
static const char * const shadow_ns_rt_tgsigqueueinfo_names[] = {
	"__arm64_sys_rt_tgsigqueueinfo", "__x64_sys_rt_tgsigqueueinfo",
	"sys_rt_tgsigqueueinfo", NULL,
};
static const char * const shadow_ns_pidfd_open_names[] = {
	"__arm64_sys_pidfd_open", "__x64_sys_pidfd_open", "sys_pidfd_open",
	NULL,
};

struct shadow_pidns_priv *shadow_ns_pidns_priv_alloc(void)
{
	struct shadow_pidns_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	mutex_init(&priv->lock);
	idr_init(&priv->idr);
	xa_init_flags(&priv->vpid_to_rpid, XA_FLAGS_ALLOC);
	xa_init(&priv->rpid_to_vpid);
	priv->adding = true;
	return priv;
}

void shadow_ns_pidns_priv_free(struct shadow_pidns_priv *priv)
{
	if (!priv)
		return;
	idr_destroy(&priv->idr);
	xa_destroy(&priv->vpid_to_rpid);
	xa_destroy(&priv->rpid_to_vpid);
	mutex_destroy(&priv->lock);
	kfree(priv);
}

/*
 * shadow_ns_pidns_register() - admit @rpid into @pidns, allocating it a
 * vpid.
 *
 * Vendored from kernel/pid.c:alloc_pid(): the vpid is allocated from
 * @pidns->idr via idr_alloc_cyclic(), starting at pid_min=1 -- so an empty
 * namespace's very first registration deterministically becomes vpid 1
 * (mirrors copy_pid_ns() assigning the namespace's child_reaper) -- and
 * wrapping back to SHADOW_NS_RESERVED_PIDS once the idr's internal cursor
 * has climbed past it, exactly matching alloc_pid()'s own comment: "init
 * really needs pid 1, but after reaching the maximum wrap back to
 * RESERVED_PIDS". Mirrors disable_pid_allocation() by refusing to admit
 * anyone once the namespace's child reaper has already exited
 * (!pidns->adding, our port of clearing PIDNS_ADDING).
 *
 * Stale-entry handling (fixes a real "wait4 sees the wrong pid" bug): a
 * task's rpid<->vpid mapping is only removed from @pidns by
 * shadow_ns_reap_stale_task_groups()'s periodic sweep (up to
 * SHADOW_NS_REAP_INTERVAL after it exits) -- deliberately not synchronously
 * on exit_group(2), since shadow_ns_hook_wait4()/shadow_ns_hook_waitid()
 * still need the mapping to translate the real pid real_sys_wait4()/
 * waitid() hands back into a vpid *after* the task has actually been
 * reaped by its parent (see shadow_ns_hook_exit_group()'s own comment). A
 * busy container can easily recycle a real host pid faster than that sweep
 * runs. shadow_ns_pidns_register() is only ever called once, synchronously, for a definitely-fresh @rpid right
 * after that task was created (shadow_ns_install_child_state(), itself
 * called immediately after the real clone()/fork() syscall returns its
 * child's tgid) -- so if @rpid already has an entry in @pidns at this
 * point, it cannot legitimately belong to the task being registered right
 * now; it can only be a leftover from a previous, already-dead task that
 * happened to reuse the same host pid number before the periodic sweep got
 * to it. The previous "rpid already has an entry -> skip" behaviour left
 * such a brand-new, live task permanently registered under that stale
 * vpid instead -- every subsequent getpid()/wait4()/... for the new task
 * then observed (or was asked to wait for) the wrong, long-dead vpid,
 * exactly the "waitpid() can't find its child" class of bug reported
 * against apt/dpkg-style short-lived subprocess-heavy workloads. Fixed by
 * always treating a pre-existing entry as stale and clearing it (mirroring
 * what the reap sweep would eventually have done) before proceeding with a
 * fresh registration for @rpid.
 */
void shadow_ns_pidns_register(struct shadow_pidns_priv *pidns, pid_t rpid)
{
	int pid_min;
	int vpid;
	void *stale;

	if (!pidns || rpid <= 0)
		return;

	mutex_lock(&pidns->lock);
	if (!pidns->adding)
		goto unlock;

	stale = xa_load(&pidns->rpid_to_vpid, rpid);
	if (stale) {
		u32 stale_vpid = (u32)xa_to_value(stale);

		xa_erase(&pidns->rpid_to_vpid, rpid);
		xa_erase(&pidns->vpid_to_rpid, stale_vpid);
		idr_remove(&pidns->idr, stale_vpid);
	}

	pid_min = (idr_get_cursor(&pidns->idr) > SHADOW_NS_RESERVED_PIDS) ?
		SHADOW_NS_RESERVED_PIDS : 1;
	vpid = idr_alloc_cyclic(&pidns->idr, NULL, pid_min, PID_MAX_LIMIT,
				 GFP_KERNEL);
	if (vpid < 0)
		goto unlock;

	if (xa_err(xa_store(&pidns->vpid_to_rpid, vpid, xa_mk_value(rpid),
			     GFP_KERNEL))) {
		idr_remove(&pidns->idr, vpid);
		goto unlock;
	}
	if (xa_err(xa_store(&pidns->rpid_to_vpid, rpid, xa_mk_value(vpid),
			     GFP_KERNEL))) {
		xa_erase(&pidns->vpid_to_rpid, vpid);
		idr_remove(&pidns->idr, vpid);
		goto unlock;
	}

	if (vpid == 1)
		pidns->child_reaper_rpid = rpid;
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
	if (v) {
		u32 vpid = xa_to_value(v);

		xa_erase(&pidns->vpid_to_rpid, vpid);
		idr_remove(&pidns->idr, vpid);
	}
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

bool shadow_ns_pidns_is_child_reaper(struct shadow_pidns_priv *pidns, pid_t rpid)
{
	bool ret;

	if (!pidns || rpid <= 0)
		return false;
	mutex_lock(&pidns->lock);
	ret = pidns->child_reaper_rpid == rpid;
	mutex_unlock(&pidns->lock);
	return ret;
}

/*
 * shadow_ns_pidns_zap() - the namespace's child reaper (vpid 1) has exited;
 * cascade termination to every other task still registered in @pidns and
 * permanently stop admitting new members.
 *
 * Vendored from kernel/pid_namespace.c:zap_pid_ns_processes(): that
 * function (a) calls disable_pid_allocation() first so nothing can join the
 * dying namespace, then (b) walks pid_ns->idr sending SIGKILL
 * (SEND_SIG_PRIV, i.e. force_sig-style, unblockable/unignorable) to every
 * task still resident, skipping ones that already have a fatal signal
 * pending. Steps (a)+(b) are reproduced faithfully below, scoped to our own
 * bookkeeping tables. What real zap_pid_ns_processes() does afterwards --
 * reparenting/reaping orphans via forget_original_parent()+kernel_wait4()
 * so the child reaper's exit only completes once the whole namespace is
 * actually gone -- relies on task_struct parent/child links this module
 * does not own and is deliberately left to the host kernel's own, already-
 * working real parent chain (see the file header).
 */
void shadow_ns_pidns_zap(struct shadow_pidns_priv *pidns, pid_t exiting_rpid)
{
	unsigned long index;
	void *entry;

	if (!pidns)
		return;

	mutex_lock(&pidns->lock);
	if (pidns->zapped) {
		mutex_unlock(&pidns->lock);
		return;
	}
	pidns->zapped = true;
	pidns->adding = false; /* disable_pid_allocation() */
	mutex_unlock(&pidns->lock);

	xa_for_each(&pidns->rpid_to_vpid, index, entry) {
		pid_t rpid = (pid_t)index;
		struct pid *pid_struct;
		struct task_struct *task;

		if (rpid == exiting_rpid)
			continue;

		pid_struct = find_get_pid(rpid);
		if (!pid_struct)
			continue;
		task = get_pid_task(pid_struct, PIDTYPE_PID);
		if (task) {
			if (!__fatal_signal_pending(task))
				send_sig(SIGKILL, task, 1);
			put_task_struct(task);
		}
		put_pid(pid_struct);
	}
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

/* Reverse of shadow_ns_pid_translate_target(): a real pid the current
 * task's namespace knows about, translated back to its vpid for return to
 * userspace (used by getpgid()/getsid(), which return a pid, not just
 * consume one).
 */
static long shadow_ns_pid_translate_result(long rpid_result)
{
	struct shadow_ns *ns;
	pid_t vpid;

	if (rpid_result <= 0)
		return rpid_result;

	ns = shadow_ns_current_pidns();
	if (!ns)
		return rpid_result;

	vpid = shadow_ns_pidns_to_vpid(ns->pid, (pid_t)rpid_result);
	shadow_ns_put(ns);
	return vpid ? vpid : rpid_result;
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

/* waitid(2) idtype values (uapi/linux/wait.h): P_ALL=0, P_PID=1, P_PGID=2.
 * Both P_PID and P_PGID carry a real pid-namespace pid number in @id.
 */
#define SHADOW_NS_P_PID		1
#define SHADOW_NS_P_PGID	2

static long shadow_ns_hook_waitid(const struct pt_regs *regs)
{
	int idtype = (int)shadow_ns_sys_arg0(regs);
	unsigned long id_arg = shadow_ns_sys_arg1(regs);
	struct pt_regs regs_copy = *regs;

	if (idtype == SHADOW_NS_P_PID || idtype == SHADOW_NS_P_PGID)
		shadow_ns_sys_set_arg1(&regs_copy, shadow_ns_pid_translate_target(id_arg));
	return real_sys_waitid(&regs_copy);
}

/*
 * shadow_ns_hook_exit_group() - immediate zap trigger.
 *
 * Real do_exit()/zap_pid_ns_processes() runs synchronously as the very
 * last thread of a pid namespace's child reaper tears down, before that
 * exit is allowed to complete. We approximate the same ordering: this
 * fires in the exit_group(2) syscall entry, before the real syscall (which
 * never returns) actually runs, so the cascade below still executes with a
 * perfectly normal, schedulable task context.
 *
 * Deliberately does NOT unregister the exiting task's own rpid<->vpid
 * mapping here: shadow_ns_hook_wait4()/shadow_ns_hook_waitid() (below) still
 * need that mapping to translate the real pid real_sys_wait4()/waitid()
 * hands back into this namespace's vpid *after* the exiting task has
 * actually been reaped by its parent, which happens strictly later than
 * this hook. Unregistering this early would make wait4() return the raw,
 * untranslated real pid instead -- exactly the symptom this file's
 * shadow_ns_pidns_register() stale-entry handling was written to fix, not
 * reintroduce. The mapping is instead cleaned up later, either by the
 * periodic reap sweep or lazily, the next time this same rpid is reused by
 * a new task (see shadow_ns_pidns_register()'s stale-entry handling).
 */
static long shadow_ns_hook_exit_group(const struct pt_regs *regs)
{
	struct shadow_ns *ns = shadow_ns_current_pidns();

	if (ns) {
		pid_t rpid = task_tgid_nr(current);

		if (shadow_ns_pidns_is_child_reaper(ns->pid, rpid))
			shadow_ns_pidns_zap(ns->pid, rpid);
		shadow_ns_put(ns);
	}
	return real_sys_exit_group(regs);
}

static long shadow_ns_hook_setpgid(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);
	unsigned long pgid_arg = shadow_ns_sys_arg1(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	shadow_ns_sys_set_arg1(&regs_copy, shadow_ns_pid_translate_target(pgid_arg));
	return real_sys_setpgid(&regs_copy);
}

static long shadow_ns_hook_getpgid(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);
	long ret;

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	ret = real_sys_getpgid(&regs_copy);
	return shadow_ns_pid_translate_result(ret);
}

static long shadow_ns_hook_getsid(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);
	long ret;

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	ret = real_sys_getsid(&regs_copy);
	return shadow_ns_pid_translate_result(ret);
}

static long shadow_ns_hook_ptrace(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg1(regs);

	shadow_ns_sys_set_arg1(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	return real_sys_ptrace(&regs_copy);
}

static long shadow_ns_hook_rt_sigqueueinfo(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long tgid_arg = shadow_ns_sys_arg0(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(tgid_arg));
	return real_sys_rt_sigqueueinfo(&regs_copy);
}

static long shadow_ns_hook_rt_tgsigqueueinfo(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long tgid_arg = shadow_ns_sys_arg0(regs);
	unsigned long tid_arg = shadow_ns_sys_arg1(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(tgid_arg));
	shadow_ns_sys_set_arg1(&regs_copy, shadow_ns_pid_translate_target(tid_arg));
	return real_sys_rt_tgsigqueueinfo(&regs_copy);
}

static long shadow_ns_hook_pidfd_open(const struct pt_regs *regs)
{
	struct pt_regs regs_copy = *regs;
	unsigned long pid_arg = shadow_ns_sys_arg0(regs);

	shadow_ns_sys_set_arg0(&regs_copy, shadow_ns_pid_translate_target(pid_arg));
	return real_sys_pidfd_open(&regs_copy);
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
static struct shadow_hook shadow_ns_exit_group_hook =
	SHADOW_HOOK(shadow_ns_exit_group_names, shadow_ns_hook_exit_group,
		    &real_sys_exit_group);
static struct shadow_hook shadow_ns_setpgid_hook =
	SHADOW_HOOK(shadow_ns_setpgid_names, shadow_ns_hook_setpgid, &real_sys_setpgid);
static struct shadow_hook shadow_ns_getpgid_hook =
	SHADOW_HOOK(shadow_ns_getpgid_names, shadow_ns_hook_getpgid, &real_sys_getpgid);
static struct shadow_hook shadow_ns_getsid_hook =
	SHADOW_HOOK(shadow_ns_getsid_names, shadow_ns_hook_getsid, &real_sys_getsid);
static struct shadow_hook shadow_ns_ptrace_hook =
	SHADOW_HOOK(shadow_ns_ptrace_names, shadow_ns_hook_ptrace, &real_sys_ptrace);
static struct shadow_hook shadow_ns_rt_sigqueueinfo_hook =
	SHADOW_HOOK(shadow_ns_rt_sigqueueinfo_names, shadow_ns_hook_rt_sigqueueinfo,
		    &real_sys_rt_sigqueueinfo);
static struct shadow_hook shadow_ns_rt_tgsigqueueinfo_hook =
	SHADOW_HOOK(shadow_ns_rt_tgsigqueueinfo_names, shadow_ns_hook_rt_tgsigqueueinfo,
		    &real_sys_rt_tgsigqueueinfo);
static struct shadow_hook shadow_ns_pidfd_open_hook =
	SHADOW_HOOK(shadow_ns_pidfd_open_names, shadow_ns_hook_pidfd_open,
		    &real_sys_pidfd_open);

struct shadow_hook *shadow_ns_pid_hooks[] = {
	&shadow_ns_getpid_hook,
	&shadow_ns_getppid_hook,
	&shadow_ns_kill_hook,
	&shadow_ns_tgkill_hook,
	&shadow_ns_tkill_hook,
	&shadow_ns_wait4_hook,
	&shadow_ns_waitid_hook,
	&shadow_ns_exit_group_hook,
	&shadow_ns_setpgid_hook,
	&shadow_ns_getpgid_hook,
	&shadow_ns_getsid_hook,
	&shadow_ns_ptrace_hook,
	&shadow_ns_rt_sigqueueinfo_hook,
	&shadow_ns_rt_tgsigqueueinfo_hook,
	&shadow_ns_pidfd_open_hook,
	NULL,
};
