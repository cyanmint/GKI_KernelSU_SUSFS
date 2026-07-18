# shadow_hook — shared ftrace hijacking helper

`shadow_hook.h` is a small, header-only ftrace-based function hooking helper
shared by `shadow_ns`, `shadow_sysvipc`, `shadow_mqueue` and `shadow_cgdevices`
(all linked into the combined `shadow_ctr.ko`; see `README.md` in this
directory for the umbrella overview).
It is what turns those modules from an ioctl API that a *patched* container
runtime must opt into, into a **transparent MITM layer**: a stock,
unpatched `containerd`/`runc`/`dockerd` calls the real syscalls
(`unshare(2)`, `setns(2)`, `msgget(2)`, `mq_open(2)`, ...) and observes working
behaviour instead of `-ENOSYS`, because the module has redirected the kernel's
own (missing/stubbed) entry points into its shadow implementation.

## How it works

Each hooked symbol is described by a `struct shadow_hook` (candidate names to
resolve, replacement function, storage for the original function pointer).
`shadow_hook_install()`:

1. Resolves the target symbol's address via the well-known
   `register_kprobe()`/`unregister_kprobe()` trick (works regardless of
   whether `kallsyms_lookup_name()` is exported).
2. Points a `struct ftrace_ops` at that address with
   `FTRACE_OPS_FL_IPMODIFY | FTRACE_OPS_FL_SAVE_REGS` and registers it.
3. When the traced function is entered, the ftrace thunk rewrites the saved
   program counter (`regs->pc` on arm64, `regs->ip` on x86-64) in the ftrace
   register snapshot so control flow jumps into our replacement instead of
   the original body.
4. The replacement keeps the original address so it can call through to
   genuine kernel behaviour (e.g. to transparently no-op on a kernel where
   the subsystem is natively present, or to chain into it after doing shadow
   bookkeeping).

Both the pre- and post-`CONFIG_DYNAMIC_FTRACE_WITH_ARGS` ftrace callback
signatures are supported so the exact same source builds unmodified across
every Android GKI kernel we target (5.10 through 6.12).

## Usage

```c
#include "../shadow_hook/shadow_hook.h"

static long (*real_sys_unshare)(const struct pt_regs *regs);

static long hook_sys_unshare(const struct pt_regs *regs)
{
	unsigned long flags = regs->regs[0];
	/* ... shadow bookkeeping ... */
	return real_sys_unshare(regs); /* or synthesize a result without calling through */
}

static const char * const unshare_names[] = { "__arm64_sys_unshare", "sys_unshare", NULL };
static struct shadow_hook unshare_hook =
	SHADOW_HOOK(unshare_names, hook_sys_unshare, &real_sys_unshare);

static struct shadow_hook *all_hooks[] = { &unshare_hook, NULL };

/* module_init: */ shadow_hook_install_all(all_hooks, "shadow_ns");
/* module_exit: */ shadow_hook_remove_all(all_hooks);
```

## Honest limitations

* A hook redirects *entry* to a syscall wrapper; it cannot fabricate struct
  layout support (e.g. `task_struct::nsproxy`) that was never compiled into
  `vmlinux`. Each module still only provides the level of behaviour documented
  in its own README (e.g. `shadow_ns` UTS isolation is fully functional, other
  namespace types remain bookkeeping-only) — `shadow_hook` only removes the
  *userspace patch* requirement to reach that behaviour, it does not upgrade
  the underlying simulation fidelity.
* If a kernel is built *with* the corresponding native subsystem
  (`CONFIG_SYSVIPC=y`, ...), installing these hooks is unnecessary and the
  modules will simply not find a real syscall for their own shadow numbers to
  register a hook for; skip loading them, or leave them loaded — they call
  through to the real implementation.
* Only one `ftrace_ops` may hold `FTRACE_OPS_FL_IPMODIFY` on a given symbol at
  a time; do not load two shadow modules that hook the same syscall.
