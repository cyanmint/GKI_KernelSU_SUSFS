// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_hijack - shared hook engine linked into the unified lkm4ctr.ko.
 *
 * Background
 * ----------
 * The five entry points exported here (shadow_hook_resolve,
 * shadow_hook_install, shadow_hook_remove, shadow_hook_install_all,
 * shadow_hook_remove_all) used to be `static inline` helpers in
 * common/shadow_hook.h, duplicated into every module TU that needed them so
 * that no shared .ko was required. Now that more than one module hooks
 * syscalls (shadow_ns, shadow_sysvipc, shadow_mqueue,
 * shadow_cgdevices), that duplication is wasteful and, more importantly, made
 * the recursion guard fragile (see below). The logic now lives here as a
 * single shared implementation; common/shadow_hook.h is a purely declarative
 * ABI header that both this module and its callers agree on.
 *
 * This subsystem has no runtime state of its own: its init/exit are no-ops
 * called from the unified lkm4ctr.ko entry/exit sequencer. Callers describe
 * each hooked symbol with a `struct shadow_hook` and hand it to
 * shadow_hook_install()/remove().
 *
 * Backend selection
 * -----------------
 * The hooking backend is chosen at *compile time* from what the target
 * kernel's own Kconfig enables:
 *   - the ftrace_ops/IPMODIFY backend when CONFIG_FUNCTION_TRACER and
 *     CONFIG_DYNAMIC_FTRACE are available; otherwise
 *   - a kprobe pre_handler backend that redirects control flow by rewriting
 *     the trapped pt_regs program counter and returning 1.
 * Several "certified"/production Android GKI boot images ship with
 * CONFIG_FUNCTION_TRACER compiled out entirely, so the kprobe backend is what
 * actually runs on stock GKI; CONFIG_KPROBES is a hard requirement of the
 * wider KernelSU/SUSFS ecosystem and is effectively always present. Both
 * backends expose the identical shadow_hook_install/remove API, so callers do
 * not know or care which one is active.
 *
 * The owner-based recursion guard
 * -------------------------------
 * A hook sits at the very first instruction of the target function, so it also
 * fires when *our own* replacement (hook->function) calls through
 * hook->original -- which resolves to the same hooked address -- to invoke
 * genuine kernel behaviour. Without a guard, that pass-through call would be
 * redirected straight back into hook->function, recursing until the kernel
 * stack overflows. The guard detects the pass-through by checking whether the
 * caller's return address lies within the module that owns the hook.
 *
 * When this logic was `static inline` and compiled into each caller,
 * THIS_MODULE naturally referred to that caller. In the merged build every
 * caller and the hook engine all live inside the same lkm4ctr.ko, so
 * hook->owner simply resolves to that unified module. The guard therefore
 * checks within_module(caller_pc, hook->owner), which is enough to recognise
 * the pass-through call. This is applied identically in both backends (the
 * ftrace thunk and the kprobe pre_handler).
 *
 * rmmod safety
 * ------------
 * Neither backend's "unregister" call waits for calls already redirected
 * into hook->function to finish executing, so an rmmod running concurrently
 * with such a call could previously free the module's memory out from under
 * it -- typically surfacing as a kernel panic some time (not necessarily
 * immediately) after rmmod, once the freed pages are reused or something
 * else runs into them. Every hook now also carries a kretprobe placed on
 * hook->function itself; the redirect points (shadow_hook_thunk(),
 * shadow_hook_pre_handler()) take a module reference with
 * try_module_get(hook->owner) immediately before redirecting, and the
 * kretprobe's return handler drops it once hook->function actually
 * returns. This keeps module_refcount() non-zero for exactly as long as any
 * redirected call is in flight anywhere in the system, so the kernel's own
 * sys_delete_module() refuses rmmod (-EBUSY, "Module ... is in use")
 * instead of racing with it -- the same protection struct
 * file_operations::owner gives an ordinary char/misc device while one of
 * its files is open. See the struct shadow_hook comment in
 * common/shadow_hook.h for the full rationale.
 */

#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ftrace.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/string.h>

#include "shadow_hook.h"

#define SHADOW_HIJACK_VERSION	"1.0"

/*
 * shadow_hook_resolve - find the runtime address of a kernel symbol.
 *
 * Uses the register_kprobe()/unregister_kprobe() trick so we don't depend on
 * the exported-ness of kallsyms_lookup_name() (largely unexported since Linux
 * 5.7). register_kprobe() internally resolves kp.addr from kp.symbol_name
 * using the kernel's own symbol table walker; we immediately unregister the
 * (never armed for our purposes) kprobe and reuse the resolved address.
 * Returns 0 if not found.
 */
unsigned long shadow_hook_resolve(const char *name)
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
EXPORT_SYMBOL_GPL(shadow_hook_resolve);

#if defined(CONFIG_ARM64)
static void shadow_hook_redirect(struct pt_regs *regs, void *function)
{
	regs->pc = (unsigned long)function;
}

/*
 * shadow_hook_caller_pc - return address of whoever called into the hooked
 * function, as seen at the very first instruction of that function (i.e.
 * before its prologue has run). On arm64 the AAPCS64 calling convention
 * passes this in the link register (x30/regs[30]).
 */
static unsigned long shadow_hook_caller_pc(const struct pt_regs *regs)
{
	return regs->regs[30];
}
#elif defined(CONFIG_X86_64)
static void shadow_hook_redirect(struct pt_regs *regs, void *function)
{
	regs->ip = (unsigned long)function;
}

/*
 * shadow_hook_caller_pc - on x86-64, CALL pushes the return address onto
 * the stack; at the hooked function's very first instruction regs->sp still
 * points directly at it.
 */
static unsigned long shadow_hook_caller_pc(const struct pt_regs *regs)
{
	return *(unsigned long *)regs->sp;
}
#else
#error "shadow_hook: unsupported architecture"
#endif

/*
 * --- rmmod safety: kretprobe-based module refcounting -------------------
 *
 * See the "rmmod safety" note on struct shadow_hook (common/shadow_hook.h)
 * for the full rationale. In short: a kretprobe placed on hook->function
 * itself (not the hooked symbol) gives us a portable, prototype-agnostic
 * way to know exactly when a redirected call finishes, regardless of
 * hook->function's real signature (pt_regs-based syscall wrappers,
 * chrdev_open()'s (inode, file) pair, ...). Its return handler drops the
 * module reference that the forward hook's redirect point acquires just
 * before redirecting, so module_refcount() stays non-zero for exactly as
 * long as a redirected call is in flight anywhere in the system.
 *
 * shadow_hook_ri_to_kretprobe() maps a struct kretprobe_instance back to
 * its owning struct kretprobe. The get_kretprobe() helper that encapsulates
 * this (correctly handling both the CONFIG_KRETPROBE_ON_RETHOOK rethook
 * form and the older struct kretprobe_holder indirection) was only added
 * upstream around v5.17; kernels at or before that (android12-5.10,
 * android13-5.10, android13-5.15, android14-5.15 in our KMI matrix) instead
 * have a plain `struct kretprobe *rp` field directly on
 * struct kretprobe_instance, with no get_kretprobe() declared at all.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
static struct kretprobe *shadow_hook_ri_to_kretprobe(struct kretprobe_instance *ri)
{
	return get_kretprobe(ri);
}
#else
static struct kretprobe *shadow_hook_ri_to_kretprobe(struct kretprobe_instance *ri)
{
	return ri->rp;
}
#endif

static int shadow_hook_retprobe_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct kretprobe *rp = shadow_hook_ri_to_kretprobe(ri);
	struct shadow_hook *hook;

	if (!rp)
		return 0;

	hook = container_of(rp, struct shadow_hook, retprobe);
	module_put(hook->owner);
	return 0;
}

/*
 * shadow_hook_retprobe_install() - best-effort; a failure here only means
 * this particular hook loses rmmod protection (shadow_hook_thunk()/
 * shadow_hook_pre_handler() check hook->retprobe_installed before taking a
 * reference, so we never acquire a module reference we could fail to
 * release). CONFIG_KRETPROBES is implied by CONFIG_KPROBES on every arch
 * this module targets, so this is not expected to fail in practice.
 */
static void shadow_hook_retprobe_install(struct shadow_hook *hook)
{
	int err;

	memset(&hook->retprobe, 0, sizeof(hook->retprobe));
	hook->retprobe.kp.addr = (kprobe_opcode_t *)hook->function;
	hook->retprobe.handler = shadow_hook_retprobe_ret;

	err = register_kretprobe(&hook->retprobe);
	if (err) {
		pr_warn("shadow_hook: register_kretprobe() failed for %s: %d; rmmod will not wait for in-flight calls to this hook\n",
			hook->resolved_name, err);
		return;
	}
	hook->retprobe_installed = true;
}

static void shadow_hook_retprobe_remove(struct shadow_hook *hook)
{
	if (!hook->retprobe_installed)
		return;
	unregister_kretprobe(&hook->retprobe);
	hook->retprobe_installed = false;
}

/*
 * shadow_hook_quiescing - module-wide "stop redirecting new calls" flag.
 *
 * Set by the safe-unload orchestration (lkm4ctr_safe_unload.c) before it
 * starts waiting for module_refcount() to drain to zero. Once set, both
 * redirect points (shadow_hook_thunk(), shadow_hook_pre_handler()) fall
 * through to genuine kernel behaviour instead of redirecting into
 * hook->function, so no *new* redirected call can start (and therefore no
 * new module reference can be acquired) while a safe-unload is in
 * progress. This is deliberately a plain bool rather than something
 * requiring hook-specific state: it is checked once per redirect attempt,
 * ahead of the (per-hook) try_module_get() call, and reset back to false if
 * a safe-unload attempt aborts (e.g. on timeout) so hooks keep working
 * normally.
 */
static bool shadow_hook_quiescing;

void shadow_hook_quiesce(bool quiesce)
{
	/* Ordinary store: this is a best-effort, eventually-consistent
	 * request flag, not a synchronisation primitive in its own right --
	 * module_refcount() is what safe-unload actually waits on.
	 */
	WRITE_ONCE(shadow_hook_quiescing, quiesce);
}
EXPORT_SYMBOL_GPL(shadow_hook_quiesce);

bool shadow_hook_is_quiescing(void)
{
	return READ_ONCE(shadow_hook_quiescing);
}
EXPORT_SYMBOL_GPL(shadow_hook_is_quiescing);

#if defined(CONFIG_FUNCTION_TRACER) && defined(CONFIG_DYNAMIC_FTRACE)

/*
 * --- ftrace_ops/IPMODIFY backend --------------------------------------
 *
 * The ftrace callback signature changed with
 * CONFIG_DYNAMIC_FTRACE_WITH_ARGS (introduced upstream for arm64/x86-64
 * around v5.19/v6.0): the fourth argument became an opaque `struct
 * ftrace_regs *` instead of `struct pt_regs *`, accessed via the
 * ftrace_get_regs() accessor. Older kernels in our supported range
 * (5.10/5.15/6.1) still use the plain pt_regs form. Both are handled here so
 * the exact same source builds unmodified against every Android GKI branch
 * we target (5.10 through 6.12).
 *
 * The recursion guard checks within_module(parent_ip, hook->owner): parent_ip
 * is the return address of whoever called the hooked function, and hook->owner
 * is the owning lkm4ctr.ko module performing the pass-through call. See
 * the file header for why.
 *
 * try_module_get(hook->owner) immediately before redirecting acquires the
 * module reference that hook->retprobe's return handler releases once
 * hook->function actually finishes -- see the "rmmod safety" note on
 * struct shadow_hook. If it fails (module already on its way out, or this
 * particular hook's retprobe never installed), fall through to genuine
 * kernel behaviour instead of redirecting.
 */
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_ARGS
static void notrace shadow_hook_thunk(unsigned long ip, unsigned long parent_ip,
				       struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
	struct pt_regs *regs = ftrace_get_regs(fregs);
	struct shadow_hook *hook = container_of(ops, struct shadow_hook, ops);

	if (!regs)
		return;
	if (within_module(parent_ip, hook->owner))
		return;
	if (shadow_hook_is_quiescing())
		return;
	if (hook->retprobe_installed && !try_module_get(hook->owner))
		return;
	shadow_hook_redirect(regs, hook->function);
}
#else
static void notrace shadow_hook_thunk(unsigned long ip, unsigned long parent_ip,
				       struct ftrace_ops *ops, struct pt_regs *regs)
{
	struct shadow_hook *hook = container_of(ops, struct shadow_hook, ops);

	if (within_module(parent_ip, hook->owner))
		return;
	if (shadow_hook_is_quiescing())
		return;
	if (hook->retprobe_installed && !try_module_get(hook->owner))
		return;
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
int shadow_hook_install(struct shadow_hook *hook)
{
	const char * const *name;
	int err;

	for (name = hook->names; *name; name++) {
		hook->address = shadow_hook_resolve(*name);
		if (hook->address) {
			hook->resolved_name = *name;
			pr_debug("shadow_hook: resolved candidate \"%s\" -> %px\n",
				 *name, (void *)hook->address);
			break;
		}
		pr_debug("shadow_hook: candidate \"%s\" not found, trying next\n", *name);
	}
	if (!hook->address)
		return -ENOENT;

	*((unsigned long *)hook->original) = hook->address;

	hook->ops.func = shadow_hook_thunk;
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS
			 | FTRACE_OPS_FL_IPMODIFY
			 | FTRACE_OPS_FL_RECURSION;

	err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
	if (err) {
		pr_debug("shadow_hook: ftrace_set_filter_ip(%s) failed: %d\n",
			 hook->resolved_name, err);
		return err;
	}

	err = register_ftrace_function(&hook->ops);
	if (err) {
		pr_debug("shadow_hook: register_ftrace_function(%s) failed: %d\n",
			 hook->resolved_name, err);
		ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
		return err;
	}

	hook->installed = true;
	shadow_hook_retprobe_install(hook);
	return 0;
}
EXPORT_SYMBOL_GPL(shadow_hook_install);

void shadow_hook_remove(struct shadow_hook *hook)
{
	if (!hook->installed)
		return;

	unregister_ftrace_function(&hook->ops);
	ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
	hook->installed = false;
	shadow_hook_retprobe_remove(hook);
}
EXPORT_SYMBOL_GPL(shadow_hook_remove);

#else /* !(CONFIG_FUNCTION_TRACER && CONFIG_DYNAMIC_FTRACE) */

/*
 * --- kprobe pre_handler backend -----------------------------------------
 *
 * Used whenever the target kernel does not have CONFIG_FUNCTION_TRACER (and
 * therefore no CONFIG_DYNAMIC_FTRACE / register_ftrace_function()) compiled
 * in, e.g. several "certified"/production GKI boot images.
 *
 * A kprobe is placed at the very first instruction of the target function.
 * When it fires, the CPU has already trapped into the kernel and @regs holds
 * the exact register state the target function would have seen. Rewriting
 * the saved program counter (arm64 pc / x86-64 ip) to our replacement
 * function and returning 1 from the pre_handler tells the kprobes core that
 * the handler has fully taken over: it must NOT single-step the original
 * (now bypassed) instruction, it should just resume the CPU with the
 * (modified) register state as-is. This is the standard technique used by
 * numerous out-of-tree hooking modules on kernels without ftrace, and is
 * fully described by the kprobes documentation's "jump/int3 based
 * probing" and "pre_handler return value" semantics.
 */
static int shadow_hook_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct shadow_hook *hook = container_of(p, struct shadow_hook, kp);

	/*
	 * The kprobe sits at the very first instruction of the hooked
	 * function, so it also fires again when *our own* replacement
	 * (hook->function) calls through hook->original -- which is simply
	 * the same hooked address -- to invoke genuine kernel behaviour.
	 * Without this check, that pass-through call would be redirected
	 * straight back into hook->function, recursing until the kernel
	 * stack overflows.
	 *
	 * Mirror the ftrace backend's within_module(parent_ip, hook->owner)
	 * guard (see shadow_hook_thunk() above) using the caller's return
	 * address, which is available at function entry (in the link
	 * register on arm64, or on the stack on x86-64) before any prologue
	 * instructions have executed. hook->owner is the merged lkm4ctr.ko
	 * module that owns the replacement function making the pass-through
	 * call.
	 * If the call came from that module, return 0 to tell the kprobes
	 * core the pre_handler has *not* taken over: it will single-step the
	 * original (untouched) instruction and resume normal execution, i.e.
	 * genuinely fall through into the target function's real body.
	 * Otherwise (a fresh, external call) redirect: return 1, which tells
	 * kprobes we have fully handled the trap ourselves (regs->pc/ip
	 * already points at hook->function) and the replaced instruction must
	 * not be single-stepped.
	 *
	 * try_module_get(hook->owner) immediately before redirecting acquires
	 * the module reference that hook->retprobe's return handler releases
	 * once hook->function actually finishes -- see the "rmmod safety"
	 * note on struct shadow_hook. If it fails (module already on its way
	 * out, or this particular hook's retprobe never installed), fall
	 * through to genuine kernel behaviour (return 0) instead of
	 * redirecting.
	 */
	if (within_module(shadow_hook_caller_pc(regs), hook->owner))
		return 0;

	if (shadow_hook_is_quiescing())
		return 0;

	if (hook->retprobe_installed && !try_module_get(hook->owner))
		return 0;

	shadow_hook_redirect(regs, hook->function);
	return 1;
}

int shadow_hook_install(struct shadow_hook *hook)
{
	const char * const *name;
	int err;

	for (name = hook->names; *name; name++) {
		hook->address = shadow_hook_resolve(*name);
		if (hook->address) {
			hook->resolved_name = *name;
			pr_debug("shadow_hook: resolved candidate \"%s\" -> %px\n",
				 *name, (void *)hook->address);
			break;
		}
		pr_debug("shadow_hook: candidate \"%s\" not found, trying next\n", *name);
	}
	if (!hook->address)
		return -ENOENT;

	*((unsigned long *)hook->original) = hook->address;

	memset(&hook->kp, 0, sizeof(hook->kp));
	hook->kp.addr = (kprobe_opcode_t *)hook->address;
	hook->kp.pre_handler = shadow_hook_pre_handler;

	err = register_kprobe(&hook->kp);
	if (err) {
		pr_debug("shadow_hook: register_kprobe(%s) failed: %d\n",
			 hook->resolved_name, err);
		return err;
	}

	hook->installed = true;
	shadow_hook_retprobe_install(hook);
	return 0;
}
EXPORT_SYMBOL_GPL(shadow_hook_install);

void shadow_hook_remove(struct shadow_hook *hook)
{
	if (!hook->installed)
		return;

	unregister_kprobe(&hook->kp);
	hook->installed = false;
	shadow_hook_retprobe_remove(hook);
}
EXPORT_SYMBOL_GPL(shadow_hook_remove);

#endif /* CONFIG_FUNCTION_TRACER && CONFIG_DYNAMIC_FTRACE */

/*
 * shadow_hook_install_all()/shadow_hook_remove_all() - convenience helpers
 * for a NULL-terminated array of `struct shadow_hook *`. A resolution
 * failure (-ENOENT) for an individual hook is logged and skipped rather than
 * aborting the whole batch, since a given kernel build may simply not have
 * that particular syscall compiled in under any name (nothing to shadow) or
 * may already provide it natively (nothing to fall back for).
 */
int shadow_hook_install_all(struct shadow_hook **hooks, const char *tag)
{
	int i, err, installed = 0;

	for (i = 0; hooks[i]; i++) {
		pr_debug("%s: attempting to install hook[%d]\n", tag, i);
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

	pr_debug("%s: hook install pass complete, %d installed\n", tag, installed);
	return installed;
}
EXPORT_SYMBOL_GPL(shadow_hook_install_all);

void shadow_hook_remove_all(struct shadow_hook **hooks)
{
	int i;

	for (i = 0; hooks[i]; i++)
		shadow_hook_remove(hooks[i]);
}
EXPORT_SYMBOL_GPL(shadow_hook_remove_all);

int shadow_hijack_init(void)
{
	/*
	 * No global state to set up: this module exists solely to host the
	 * exported shadow_hook_* implementation. Loading it makes those
	 * symbols available to the hooking modules that depend on it.
	 */
	pr_info("shadow_hijack: loaded (shared shadow_hook implementation)\n");
	return 0;
}

void shadow_hijack_exit(void)
{
	pr_info("shadow_hijack: unloaded\n");
}
