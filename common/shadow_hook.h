/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_hook - tiny ftrace/kprobe-based function hijacking ABI shared by the
 * shadow_ns / shadow_sysvipc / shadow_mqueue / shadow_cgdevices subsystems.
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
 * Linux 5.7, commit 0bd476e6c671) kallsyms_lookup_name(): the implementation
 * uses the well-known register_kprobe() trick instead. See shadow_hijack.c
 * for the details.
 *
 * Where the implementation lives (this changed!)
 * ----------------------------------------------
 * This header used to be intentionally include-only, with every helper marked
 * `static inline` so each caller TU got its own private copy. That is no
 * longer the case: now that more than one subsystem needs the hook logic, the
 * single source of truth lives in
 * shadow_ctr/shadow_hijack/shadow_hijack.c inside the merged shadow_ctr.ko
 * module, which still EXPORT_SYMBOL_GPL()s the five entry points declared at
 * the bottom of this file for any future external consumers. This header is
 * now purely declarative: it defines the ABI (struct shadow_hook, the
 * SHADOW_HOOK() initialiser macro, and the extern function prototypes) that
 * the unified module's subsystems share.
 *
 * Notes preserved from the original design (still true, still worth reading):
 * - The ftrace path is only usable for functions ftrace can trace (must have
 *   an mcount/patchable call site, i.e. anything not marked notrace and
 *   built with CONFIG_FUNCTION_TRACER). All in-tree syscall wrappers qualify
 *   *when that option is enabled*.
 * - Several real-world "certified"/production Android GKI boot images ship
 *   with CONFIG_FUNCTION_TRACER (and therefore CONFIG_DYNAMIC_FTRACE,
 *   register_ftrace_function(), ...) compiled out entirely. The shadow_hijack
 *   subsystem therefore picks its hooking backend at *compile time*: the ftrace
 *   ops/IPMODIFY backend when CONFIG_FUNCTION_TRACER (and
 *   CONFIG_DYNAMIC_FTRACE) are available, otherwise a kprobe pre_handler
 *   backend. Both backends expose the identical shadow_hook_install/remove
 *   API declared below, so none of the calling modules need to know or care
 *   which one is active. The struct shadow_hook layout below likewise selects
 *   its backend field (ops vs kp) on the same compile-time condition, and is
 *   shared verbatim by all unified-module subsystems.
 * - IPMODIFY/kprobe hooks are exclusive per-symbol: only one shadow_hook may
 *   target a given symbol at a time. This is fine for our use (each module
 *   owns a disjoint set of syscalls).
 */

#ifndef _SHADOW_HOOK_H
#define _SHADOW_HOOK_H

#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ftrace.h>
#include <linux/module.h>
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
 * @owner:    the module that owns this hook -- i.e. the module whose
 *            replacement @function performs the pass-through call back into
 *            @original. The recursion guard uses within_module(caller_pc,
 *            @owner) to distinguish that pass-through call (which must fall
 *            through to the genuine function) from a fresh external call
 *            (which must be redirected). In the merged build this is the
 *            unified shadow_ctr.ko module for every subsystem; the
 *            SHADOW_HOOK() macro plumbs it through automatically from each
 *            caller's TU.
 * @address:  resolved address of the hooked symbol.
 * @ops:      ftrace_ops instance driving the hook (ftrace backend only).
 * @kp:       kprobe instance driving the hook (kprobe backend only).
 * @installed: whether the hook is currently active.
 * @retprobe: kretprobe placed on @function itself (regardless of which
 *            forward-hook backend is active), used purely to make module
 *            unload safe -- see the "rmmod safety" note below.
 * @retprobe_installed: whether @retprobe is currently registered.
 *
 * rmmod safety
 * ------------
 * A redirected call spends real time executing inside @function (part of
 * this module's .text), on a CPU that may be entirely unrelated to whoever
 * calls shadow_hook_remove()/rmmod. Unregistering the forward hook (ftrace
 * or kprobe) only stops *new* calls from being redirected; it does not wait
 * for calls already in flight to finish, so a concurrent rmmod could free
 * the module's memory while another CPU is still executing inside
 * @function, corrupting the kernel (typically observed as a panic seconds
 * after rmmod, once something happens to run into the freed pages).
 *
 * @retprobe closes that window using the same mechanism relied upon
 * everywhere else in the kernel to keep a module alive while it is in use:
 * module reference counting. shadow_hook_install() places @retprobe on
 * @function and its return handler calls module_put(@owner); the forward
 * hook's redirect point (shadow_hook_thunk()/shadow_hook_pre_handler())
 * calls try_module_get(@owner) immediately before redirecting into
 * @function, and skips the redirect (falling back to genuine kernel
 * behaviour) if that fails, which only happens once the module is already
 * on its way out. With this in place, module_refcount() is non-zero for as
 * long as any redirected call is in flight, so the kernel's own
 * sys_delete_module() refuses rmmod (-EBUSY, "Module ... is in use")
 * instead of racing with it -- exactly the same protection a misc/char
 * device gets from struct file_operations::owner while a file is open.
 */
struct shadow_hook {
	const char * const	*names;
	const char		*resolved_name;
	void			*function;
	void			*original;
	struct module		*owner;
	unsigned long		address;
#if defined(CONFIG_FUNCTION_TRACER) && defined(CONFIG_DYNAMIC_FTRACE)
	struct ftrace_ops	ops;
#else
	struct kprobe		kp;
#endif
	bool			installed;
	struct kretprobe	retprobe;
	bool			retprobe_installed;
};

/*
 * SHADOW_HOOK - static initialiser for a struct shadow_hook.
 *
 * @owner is deliberately not a macro parameter: it is hard-wired to
 * THIS_MODULE so that each expansion picks up the *calling* translation
 * unit's own module. In the merged build that is always shadow_ctr.ko, which
 * is sufficient for the recursion guard's "call originated from inside the
 * unified module" check.
 */
#define SHADOW_HOOK(_names, _function, _original_storage)		\
	{								\
		.names    = (_names),					\
		.function = (_function),				\
		.original = (_original_storage),			\
		.owner    = THIS_MODULE,				\
	}

/*
 * The hook implementation lives in shadow_ctr/shadow_hijack/shadow_hijack.c
 * and is reached through these entry points. See that file for their full
 * contracts.
 *
 * shadow_hook_resolve()      - resolve a kernel symbol's runtime address (0 if
 *                              not found), via the register_kprobe() trick.
 * shadow_hook_install()      - resolve @hook->names and start redirecting; on
 *                              success *(void **)hook->original holds the real
 *                              function. Returns 0, -ENOENT if no candidate
 *                              name resolves, or another negative errno.
 * shadow_hook_remove()       - stop redirecting a single hook.
 * shadow_hook_install_all()  - install a NULL-terminated array of hooks,
 *                              skipping (-ENOENT) ones whose symbol is absent;
 *                              returns the count installed, or negative errno.
 * shadow_hook_remove_all()   - remove a NULL-terminated array of hooks.
 */
unsigned long shadow_hook_resolve(const char *name);
int shadow_hook_install(struct shadow_hook *hook);
void shadow_hook_remove(struct shadow_hook *hook);
int shadow_hook_install_all(struct shadow_hook **hooks, const char *tag);
void shadow_hook_remove_all(struct shadow_hook **hooks);

#endif /* _SHADOW_HOOK_H */
