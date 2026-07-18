# shadow_hijack — shared function-hijacking implementation

`shadow_hijack.ko` is a small, standalone kernel module that hosts the
`shadow_hook_resolve()/install()/remove()/install_all()/remove_all()`
implementation shared by every syscall-hooking module in the `shadow_ctr`
family: `shadow_ns_base`, `shadow_ns_uts`, `shadow_sysvipc`, `shadow_mqueue`
and `shadow_cgdevices`. See `../common/shadow_hook.h` for the full ABI
contract (struct layout, `SHADOW_HOOK()` initialiser macro, function
prototypes) that this module implements and every caller includes.

## Why a separate module?

Earlier revisions kept this logic as `static inline` helpers directly in
`shadow_hook.h`, so each module got its own private copy compiled in and no
shared `.ko` was required. Now that five different modules hook syscalls,
that duplication was wasteful and, more importantly, made the recursion guard
(see below) fragile. The implementation now lives in exactly one place —
this module — and `shadow_hook.h` is a purely declarative ABI header shared
by `shadow_hijack.ko` and its callers.

Every hooking module must therefore:

* be built against `shadow_hijack`'s `Module.symvers` (via
  `KBUILD_EXTRA_SYMBOLS`, see each module's own `Makefile`), and
* be `insmod`'d **after** `shadow_hijack.ko` at runtime, since it resolves
  `shadow_hijack`'s `EXPORT_SYMBOL_GPL()`'d entry points.

`shadow_hijack.ko` itself has no runtime state and no other module
dependency: its `module_init`/`module_exit` are no-ops that exist purely to
make the exported symbols available.

## Backend selection (ftrace vs. kprobe)

The hooking backend is chosen at **compile time**, from what the target
kernel's own Kconfig enables:

* the `ftrace_ops`/`IPMODIFY` backend when `CONFIG_FUNCTION_TRACER` and
  `CONFIG_DYNAMIC_FTRACE` are both available, otherwise
* a `kprobe` `pre_handler` backend that redirects control flow by rewriting
  the trapped `pt_regs` program counter (`regs->pc` on arm64, `regs->ip` on
  x86-64) and returning 1 to tell the kprobes core it must not single-step
  the original instruction.

Several "certified"/production Android GKI boot images ship with
`CONFIG_FUNCTION_TRACER` compiled out entirely, so the kprobe backend is what
actually runs on stock GKI; `CONFIG_KPROBES` is a hard requirement of the
wider KernelSU/SUSFS ecosystem and is effectively always present. Both
backends expose the identical `shadow_hook_install()`/`shadow_hook_remove()`
API, so callers do not know or care which one is active.

## The owner-based recursion guard

A hook sits at the very first instruction of the target function, so it also
fires when the caller's own replacement function (`hook->function`) calls
through `hook->original` — which resolves to the exact same hooked address —
to invoke genuine kernel behaviour. Without a guard, that pass-through call
would be redirected straight back into `hook->function`, recursing until the
kernel stack overflows.

The guard detects the pass-through by checking whether the caller's return
address lies within the module that *owns* the hook
(`within_module(caller_pc, hook->owner)`). Now that the hook implementation
lives in `shadow_hijack.ko` rather than being compiled into each caller,
`THIS_MODULE` inside this file would refer to `shadow_hijack.ko` itself —
which would be wrong, since the pass-through call is always made from the
*calling* module's replacement function (e.g. `shadow_sysvipc`'s
`svipc_hook_msgget()` calling `real_sys_msgget()`), never from code inside
`shadow_hijack.ko`. `struct shadow_hook::owner` is therefore populated with
the *caller's* `THIS_MODULE` by the `SHADOW_HOOK()` initialiser macro (see
`shadow_hook.h`), and both backends here check
`within_module(caller_pc, hook->owner)` against that caller-owned value. This
is the fix that made the recursion guard work correctly again after the
extraction into a standalone module.

## Building

Dual-purpose kbuild `Makefile`, like every other module in this family:

```sh
make KDIR=/path/to/kernel/build
make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1     # Android GKI cross build
```

Out-of-tree modules for GKI **must** be built inside the matching
`ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image against its real
`vmlinux`/`Module.symvers`, not a bare `gki_defconfig` tree — see
[`../README.md`](../README.md) and
[`../../.github/workflows/build-shadow-ctr.yml`](../../.github/workflows/build-shadow-ctr.yml).

Build (and `insmod`) `shadow_hijack.ko` **before** any module that depends on
it.
