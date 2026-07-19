# shadow_ctr_checker

`shadow_ctr_checker.ko` is a **pure, standalone diagnostics module** for the
`shadow_ctr` family. It has **no build-time or load-time dependency on any
other shadow_ctr module** — it can be built and `insmod`'d entirely on its
own, in any order, regardless of which (if any) other `shadow_*` modules are
present. It registers a read-only character device, `/dev/shadow_ctr_checker`
(mode `0444`); reading it produces a plain, greppable, one-line-per-check
report of which container-relevant kernel features are available and whether
that support is native to the kernel (compile-time) or, for namespaces, would
be provided by `shadow_ns.ko` if it happens to be loaded.

```sh
insmod shadow_ctr_checker.ko
cat /dev/shadow_ctr_checker
```

Example output:

```
shadow_ctr_checker v2.0 - shadow container-support status
----------------------------------------
mqueue: not supported (not builtin); check `lsmod`/`/proc/modules` for a shadow_* provider
sysvipc: not supported (not builtin); check `lsmod`/`/proc/modules` for a shadow_* provider
cgroup_device: supported (builtin)
overlay2: supported (module: overlay)
# namespaces (task explicitly requests net/pid/ipc/uts; mnt/user/cgroup shown for completeness)
ns_net: not supported (not builtin); shadow_ns.ko provides bookkeeping-only fallback if loaded - check lsmod
ns_pid: not supported (not builtin); shadow_ns.ko provides real isolation if loaded - check lsmod
ns_ipc: not supported (not builtin); shadow_ns.ko provides bookkeeping-only fallback if loaded - check lsmod
ns_uts: not supported (not builtin); shadow_ns.ko provides real isolation if loaded - check lsmod
ns_mnt: supported (builtin)
ns_user (user namespace): supported (builtin)
ns_cgroup (proxy: CONFIG_CGROUPS): supported (builtin)
```

The report is generated fresh on every `open()`.

## What it checks

| Line                | Native (compile-time) check          | Namespace fallback (if not builtin) |
|---------------------|--------------------------------------|------------------------|
| `mqueue`            | `IS_ENABLED(CONFIG_POSIX_MQUEUE)`    | n/a (see below) |
| `sysvipc`           | `IS_ENABLED(CONFIG_SYSVIPC)`         | n/a (see below) |
| `cgroup_device`     | `IS_ENABLED(CONFIG_CGROUP_DEVICE)`   | n/a (see below) |
| `overlay2`          | `get_fs_type("overlay")` ground truth| distinguishes builtin / loadable module |
| `ns_net`            | `IS_ENABLED(CONFIG_NET_NS)`          | bookkeeping-only, if `shadow_ns.ko` loaded |
| `ns_pid`            | `IS_ENABLED(CONFIG_PID_NS)`          | real isolation, if `shadow_ns.ko` loaded |
| `ns_ipc`            | `IS_ENABLED(CONFIG_IPC_NS)`          | bookkeeping-only, if `shadow_ns.ko` loaded |
| `ns_uts`            | `IS_ENABLED(CONFIG_UTS_NS)`          | real isolation, if `shadow_ns.ko` loaded |
| `ns_mnt`            | `IS_ENABLED(CONFIG_NAMESPACES)` (proxy; no dedicated `CONFIG_MNT_NS` symbol exists) | bookkeeping-only, if `shadow_ns.ko` loaded (only if `CONFIG_NAMESPACES=n`, effectively never in practice) |
| `ns_user`           | `IS_ENABLED(CONFIG_USER_NS)`         | real isolation, if `shadow_ns.ko` loaded (called out explicitly) |
| `ns_cgroup`         | `IS_ENABLED(CONFIG_CGROUPS)` (proxy) | bookkeeping-only, if `shadow_ns.ko` loaded (only if `CONFIG_CGROUPS=n`, rare) |

`mqueue`/`sysvipc`/`cgroup_device` have no runtime "which `.ko` provides it"
column (see "Why there is no cross-module runtime detection at all" below);
use `lsmod`/`cat /proc/modules` to see whether
`shadow_mqueue`/`shadow_sysvipc`/`shadow_cgdevices` is loaded.

`net`, `pid`, `ipc` and `uts` are the four namespaces the task explicitly asks
about; `mnt`, `user` and `cgroup` are included for completeness. `user` gets
its own clearly labelled line. cgroup namespace has no dedicated Kconfig gate
in mainline, so `CONFIG_CGROUPS` is used as its "builtin" proxy (documented in
the report line).

## Why `IS_ENABLED()` is trustworthy here

Because this module is compiled against the **exact target kernel's config**
via the same `ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image as every other
`shadow_*` module, its compile-time `IS_ENABLED(CONFIG_*)` results reflect the
real running kernel — there is no config-vs-behaviour mismatch of the kind the
old `/proc/config.gz` spoofing risked. This is also what lets the namespace
lines describe what `shadow_ns.ko` *would* provide without ever calling into
`shadow_ns` itself: `shadow_ns.c` computes whether it simulates a given
namespace type (and whether that simulation is real vs. bookkeeping-only)
using the exact same `IS_ENABLED(CONFIG_{UTS,IPC,USER,PID,NET}_NS)` checks —
since both modules are built against the identical config, the two
independently-computed answers are guaranteed to agree.

For **overlayfs specifically** the checker prefers `get_fs_type("overlay")`
over `IS_ENABLED(CONFIG_OVERLAY_FS)`, because `get_fs_type()` is the
ground-truth answer to "will `mount(2)` of an overlay actually succeed": overlay
could be builtin (`=y`) or provided by a genuine loadable `overlay.ko`. The
checker inspects the returned `file_system_type->owner` to distinguish builtin
(owner `NULL`) from a module, and reports the owning module's name. The
reference `get_fs_type()` takes is released with `module_put()`.

## Why there is no cross-module runtime detection at all

An earlier revision resolved every cross-module check —
`shadow_mqueue_is_active`, `shadow_sysvipc_is_active`,
`shadow_cgdevices_is_active`, and a namespace-registry module's
`*_type_loaded` / `*_type_real` query API — purely at runtime via
`symbol_get()` / `symbol_put()` (backed by `__symbol_get()`/
`__symbol_put()`), so the checker had **no** build-time dependency
(`KBUILD_EXTRA_SYMBOLS` / `Module.symvers`) on any other module and would
load standalone regardless of which subset of them was present.

However, `__symbol_get()`/`__symbol_put()` are themselves trimmed from
production GKI kernels' exported-symbol table (`CONFIG_TRIM_UNUSED_KSYMS`
drops `EXPORT_SYMBOL` entries unreferenced by any built-in code, even though
the functions remain compiled into the kernel image). Merely *referencing*
`symbol_get()`/`symbol_put()` anywhere in the module — even in a branch never
taken at runtime — makes the whole module fail `insmod` with "Unknown symbol
__symbol_get"/"Unknown symbol __symbol_put" (surfaced by `insmod` as
`-ENOENT`, i.e. "No such file or directory"), because the kernel's module
loader resolves every referenced symbol up front before the module can load
at all, regardless of runtime control flow.

A later revision instead took an ordinary build+load-time dependency on
`shadow_ns`'s `EXPORT_SYMBOL_GPL` query API via `KBUILD_EXTRA_SYMBOLS`. That
has been removed too: this checker is meant to be a pure, dependency-free
diagnostics tool, so it now derives the namespace lines purely from its own
`IS_ENABLED()` checks (see "Why `IS_ENABLED()` is trustworthy here" above)
instead of calling into `shadow_ns` at all. As a result it no longer knows
*whether* `shadow_ns.ko` is actually loaded right now — only what it *would*
provide for a non-builtin type if loaded — so use `lsmod`/`/proc/modules` to
confirm `shadow_ns.ko` itself is present.

`shadow_mqueue`/`shadow_sysvipc`/`shadow_cgdevices`'s "which `.ko`" runtime
detection column was dropped for the same reason (no safe symbol-free query
mechanism is available); their compile-time `IS_ENABLED()` line is
unaffected. Use `lsmod`/`/proc/modules` to check whether an optional module is
loaded.

## Build

```sh
make -C /path/to/kernel/build M="$PWD" modules   # produces shadow_ctr_checker.ko
# or
make KDIR=/path/to/kernel/build
```

No `KBUILD_EXTRA_SYMBOLS`/`Module.symvers` from any other module is needed —
this module has zero build dependencies. Build inside the matching
`ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image so its `IS_ENABLED()`
checks reflect the real target kernel config.
