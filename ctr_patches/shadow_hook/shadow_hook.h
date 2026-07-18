/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_hook - tiny ftrace-based function hijacking helper shared by the
 * shadowns / shadow_sysvipc / shadow_mqueue / shadow_cgdevices modules.
 *
 * Motivation
 * ----------
 * The earlier revisions of the shadow_* modules exposed their functionality
 * only through ioctls on a private /dev/shadow_* misc device, which required
 * a *patched* container runtime (containerd/runc) that knew to open that
 * device and call the ioctls instead of the real syscalls. That is safe but
 * requires out-of-tree changes to userspace.
 *
 * shadow_hook removes that requirement: it lets a module intercept the
 * *real* kernel entry points (syscall wrappers, LSM-adjacent permission
 * checks, ...) that are missing or stubbed out because the corresponding
 * Kconfig option (CONFIG_SYSVIPC, CONFIG_POSIX_MQUEUE, CONFIG_*_NS,
 * CONFIG_CGROUP_DEVICE, ...) was left disabled, and transparently redirect
 * them into the module's shadow implementation. An unmodified, stock
 * containerd/runc/dockerd binary that calls unshare(2)/setns(2)/msgget(2)/
 * mq_open(2)/... observes a working call instead of -ENOSYS, with no
 * userspace changes at all.
 *
 * This is a classic ftrace-ops based function hook: the target symbol's
 * entry is patched by the ftrace subsystem (the same mechanism used by
 * function tracing / live patching) to redirect into our thunk, which in
 * turn diverts control flow (by rewriting the traced instruction pointer in
 * the saved register state) into our replacement function. The replacement
 * keeps a pointer to the original function so it can still call through to
 * genuine kernel behaviour when the shadow implementation wants to
 * transparently fall back (e.g. the syscall is not one we want to shadow on
 * this particular kernel because CONFIG_SYSVIPC=y after all).
 *
 * Symbols are resolved without relying on the (largely unexported since
 * Linux 5.7, commit 0bd476e6c671) kallsyms_lookup_name(): we use the
 * well-known register_kprobe() trick instead. register_kprobe() internally
 * resolves kp.addr from kp.symbol_name using the kernel's own symbol table
 * walker, which remains available regardless of kallsyms_lookup_name()
 * export status; we immediately unregister the (never armed for our
 * purposes) kprobe and reuse the resolved address for the ftrace hook.
 *
 * Caveats (please read before extending this file)
 * -------------------------------------------------
 * - Only usable for functions ftrace can trace (must have an mcount/patchable
 *   call site, i.e. anything not marked notrace and built with
 *   CONFIG_FUNCTION_TRACER). All in-tree syscall wrappers qualify.
 * - IPMODIFY hooks are exclusive per-symbol: only one shadow_hook may target
 *   a given symbol at a time. This is fine for our use (each module owns a
 *   disjoint set of syscalls).
 * - This header is intentionally include-only (all helpers are `static`) so
 *   each standalone module TU gets its own private copy; there is no shared
 *   .ko to link against, keeping every shadow_* module fully self-contained.
 */

#ifndef _SHADOW_HOOK_H
#define _SHADOW_HOOK_H

#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ftrace.h>
#include <linux/version.h>
#include <linux/string.h>

/*
 * struct shadow_hook - one hooked symbol.
 * @names:    NULL-terminated list of candidate symbol names to try, in
 *            order. Different kernel configs/versions can export the same
 *            syscall under slightly different wrapper names (e.g. the arm64
 *            SYSCALL_WRAPPER prefix "__arm64_sys_xxx" vs. a plain "sys_xxx"
 *            fallback), so we try each until one resolves.
 * @resolved_name: the candidate name that actually resolved (for logging).
 * @function: our replacement function. Must have the exact same prototype
 *            as the original (typically `long fn(const struct pt_regs *)`
 *            for arm64/x86-64 syscall wrappers).
 * @original: address of the real function, filled in by
 *            shadow_hook_install(); call through *this* to invoke genuine
 *            kernel behaviour.
 * @address:  resolved address of the hooked symbol.
 * @ops:      ftrace_ops instance driving the hook.
 * @installed: whether register_ftrace_function() succeeded.
 */
struct shadow_hook {
	const char * const	*names;
	const char		*resolved_name;
	void			*function;
	void			*original;
	unsigned long		address;
	struct ftrace_ops	ops;
	bool			installed;
};

#define SHADOW_HOOK(_names, _function, _original_storage)		\
	{								\
		.names    = (_names),					\
		.function = (_function),				\
		.original = (_original_storage),			\
	}

/*
 * shadow_hook_resolve - find the runtime address of a kernel symbol.
 *
 * Uses the register_kprobe()/unregister_kprobe() trick so we don't depend on
 * the exported-ness of kallsyms_lookup_name(). Returns 0 if not found.
 */
static inline unsigned long shadow_hook_resolve(const char *name)
{
	struct kprobe kp;
	unsigned long addr;
	int ret;

	memset(&kp, 0, sizeof(kp));
	kp.symbol_name = name;

	ret = register_kprobe(&kp);
	if (ret < 0)
		return 0;

	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

#if defined(CONFIG_ARM64)
static inline void shadow_hook_redirect(struct pt_regs *regs, void *function)
{
	regs->pc = (unsigned long)function;
}
#elif defined(CONFIG_X86_64)
static inline void shadow_hook_redirect(struct pt_regs *regs, void *function)
{
	regs->ip = (unsigned long)function;
}
#else
#error "shadow_hook: unsupported architecture"
#endif

/*
 * The ftrace callback signature changed with
 * CONFIG_DYNAMIC_FTRACE_WITH_ARGS (introduced upstream for arm64/x86-64
 * around v5.19/v6.0): the fourth argument became an opaque `struct
 * ftrace_regs *` instead of `struct pt_regs *`, accessed via the
 * ftrace_get_regs() accessor. Older kernels in our supported range
 * (5.10/5.15/6.1) still use the plain pt_regs form. Both are handled here so
 * the exact same source builds unmodified against every Android GKI branch
 * we target (5.10 through 6.12).
 */
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_ARGS
static void notrace shadow_hook_thunk(unsigned long ip, unsigned long parent_ip,
				       struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
	struct pt_regs *regs = ftrace_get_regs(fregs);
	struct shadow_hook *hook = container_of(ops, struct shadow_hook, ops);

	if (!regs)
		return;
	if (!within_module(parent_ip, THIS_MODULE))
		shadow_hook_redirect(regs, hook->function);
}
#else
static void notrace shadow_hook_thunk(unsigned long ip, unsigned long parent_ip,
				       struct ftrace_ops *ops, struct pt_regs *regs)
{
	struct shadow_hook *hook = container_of(ops, struct shadow_hook, ops);

	if (!within_module(parent_ip, THIS_MODULE))
		shadow_hook_redirect(regs, hook->function);
}
#endif

/*
 * shadow_hook_install - resolve @hook->names and start redirecting calls.
 *
 * On success, *(void **)hook->original holds the genuine function's address
 * (call through it with the same prototype to invoke real kernel code), and
 * hook->installed is true.
 *
 * Returns 0 on success, negative errno otherwise. It is not an error for the
 * symbol to be missing entirely (returns -ENOENT) so callers can simply skip
 * shadowing a syscall that a particular kernel build already implements
 * natively (CONFIG_SYSVIPC=y, etc.) or does not expose at all.
 */
static inline int shadow_hook_install(struct shadow_hook *hook)
{
	const char * const *name;
	int err;

	for (name = hook->names; *name; name++) {
		hook->address = shadow_hook_resolve(*name);
		if (hook->address) {
			hook->resolved_name = *name;
			break;
		}
	}
	if (!hook->address)
		return -ENOENT;

	*((unsigned long *)hook->original) = hook->address;

	hook->ops.func = shadow_hook_thunk;
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS
			 | FTRACE_OPS_FL_IPMODIFY
			 | FTRACE_OPS_FL_RECURSION;

	err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
	if (err)
		return err;

	err = register_ftrace_function(&hook->ops);
	if (err) {
		ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
		return err;
	}

	hook->installed = true;
	return 0;
}

static inline void shadow_hook_remove(struct shadow_hook *hook)
{
	if (!hook->installed)
		return;

	unregister_ftrace_function(&hook->ops);
	ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
	hook->installed = false;
}

/*
 * shadow_hook_install_all()/shadow_hook_remove_all() - convenience helpers
 * for a NULL-terminated array of `struct shadow_hook *`. A resolution
 * failure (-ENOENT) for an individual hook is logged and skipped rather than
 * aborting the whole batch, since a given kernel build may simply not have
 * that particular syscall compiled in under any name (nothing to shadow) or
 * may already provide it natively (nothing to fall back for).
 */
static inline int shadow_hook_install_all(struct shadow_hook **hooks, const char *tag)
{
	int i, err, installed = 0;

	for (i = 0; hooks[i]; i++) {
		err = shadow_hook_install(hooks[i]);
		if (err == -ENOENT) {
			pr_info("%s: symbol for hook[%d] not found, skipping\n", tag, i);
			continue;
		}
		if (err) {
			pr_err("%s: failed to install hook[%d]: %d\n", tag, i, err);
			return err;
		}
		pr_info("%s: hooked %s at %px\n", tag, hooks[i]->resolved_name,
			(void *)hooks[i]->address);
		installed++;
	}

	return installed;
}

static inline void shadow_hook_remove_all(struct shadow_hook **hooks)
{
	int i;

	for (i = 0; hooks[i]; i++)
		shadow_hook_remove(hooks[i]);
}

#endif /* _SHADOW_HOOK_H */
