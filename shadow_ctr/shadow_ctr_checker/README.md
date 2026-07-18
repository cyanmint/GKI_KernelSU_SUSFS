# shadow_ctr_checker

`shadow_ctr_checker.ko` is a **diagnostics module** for the `shadow_ctr`
family (build/load dependency: `shadow_ns_base` only). It registers a
read-only character device, `/dev/shadow_ctr_checker` (mode `0444`); reading
it produces a plain, greppable, one-line-per-check report of which
container-relevant kernel features are available and, for each, whether that
support is native to the kernel or provided by `shadow_ns_base`'s namespace
bookkeeping.

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
overlay2: supported (shadow_overlay2.ko)
# namespaces (task explicitly requests net/pid/ipc/uts; mnt/user/cgroup shown for completeness)
ns_net: supported (shadow_ns bookkeeping)
ns_pid: not supported
ns_ipc: not supported
ns_uts: supported (shadow_ns real)
ns_mnt: not supported
ns_user (user namespace): supported (builtin)
ns_cgroup (proxy: CONFIG_CGROUPS): supported (builtin)
```

The report is generated fresh on every `open()`.

## What it checks

| Line                | Native (compile-time) check          | Shadow (runtime) check |
|---------------------|--------------------------------------|------------------------|
| `mqueue`            | `IS_ENABLED(CONFIG_POSIX_MQUEUE)`    | none (see below) |
| `sysvipc`           | `IS_ENABLED(CONFIG_SYSVIPC)`         | none (see below) |
| `cgroup_device`     | `IS_ENABLED(CONFIG_CGROUP_DEVICE)`   | none (see below) |
| `overlay2`          | `get_fs_type("overlay")` ground truth| distinguishes builtin / `shadow_overlay2.ko` / other module |
| `ns_net`            | `IS_ENABLED(CONFIG_NET_NS)`          | `shadow_ns_base` reports NET loaded/real |
| `ns_pid`            | `IS_ENABLED(CONFIG_PID_NS)`          | … PID |
| `ns_ipc`            | `IS_ENABLED(CONFIG_IPC_NS)`          | … IPC |
| `ns_uts`            | `IS_ENABLED(CONFIG_UTS_NS)`          | … UTS |
| `ns_mnt`            | `IS_ENABLED(CONFIG_MNT_NS)`          | … MNT |
| `ns_user`           | `IS_ENABLED(CONFIG_USER_NS)`         | … USER (called out explicitly) |
| `ns_cgroup`         | `IS_ENABLED(CONFIG_CGROUPS)` (proxy) | … CGROUP |

`mqueue`/`sysvipc`/`cgroup_device` no longer have a runtime "which `.ko`
provides it" column (see "Why there is no cross-module `symbol_get()` any
more" below); use `lsmod`/`cat /proc/modules` to see whether
`shadow_mqueue`/`shadow_sysvipc`/`shadow_cgdevices` is loaded.

`net`, `pid`, `ipc` and `uts` are the four namespaces the task explicitly asks
about; `mnt`, `user` and `cgroup` are included for completeness since the same
`shadow_ns_base` query API already covers them. `user` gets its own clearly
labelled line. cgroup namespace has no dedicated Kconfig gate in mainline, so
`CONFIG_CGROUPS` is used as its "builtin" proxy (documented in the report
line).

## Why `IS_ENABLED()` is trustworthy here

Because this module is compiled against the **exact target kernel's config**
via the same `ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image as every other
`shadow_*` module, its compile-time `IS_ENABLED(CONFIG_*)` results reflect the
real running kernel — there is no config-vs-behaviour mismatch of the kind the
old `/proc/config.gz` spoofing risked.

For **overlayfs specifically** the checker prefers `get_fs_type("overlay")`
over `IS_ENABLED(CONFIG_OVERLAY_FS)`, because `get_fs_type()` is the
ground-truth answer to "will `mount(2)` of an overlay actually succeed": overlay
could be builtin (`=y`), a genuine loadable `overlay.ko`, or provided by
`shadow_overlay2.ko`. The checker inspects the returned
`file_system_type->owner` to distinguish builtin (owner `NULL`) from a module,
and compares the owning module's name to identify `shadow_overlay2`. The
reference `get_fs_type()` takes is released with `module_put()`.

## Why there is no cross-module `symbol_get()` any more

An earlier revision resolved every cross-module check —
`shadow_mqueue_is_active`, `shadow_sysvipc_is_active`,
`shadow_cgdevices_is_active`, and `shadow_ns_base`'s
`shadow_ns_base_type_loaded` / `shadow_ns_base_type_real` — purely at runtime
via `symbol_get()` / `symbol_put()` (backed by `__symbol_get()`/
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

`shadow_ns_base` is foundational — per the top-level
[`../README.md`](../README.md) load order it is always loaded right after
`shadow_hijack`, before every other `shadow_*` module including this checker,
which loads last — so this module now takes an ordinary build+load-time
dependency on `shadow_ns_base`'s `EXPORT_SYMBOL_GPL` query API instead, via
`KBUILD_EXTRA_SYMBOLS`, matching the pattern `shadow_ns_uts`/`shadow_ns_net`/…
already use for the same API.

`shadow_mqueue`/`shadow_sysvipc`/`shadow_cgdevices` are optional and
independently loadable, so — unlike `shadow_ns_base` — they are **not** given
a hard Kbuild dependency (that would make this checker itself fail to load
whenever one of them wasn't present); their "which `.ko`" runtime detection
column has instead been dropped entirely, since no safe symbol-free query
mechanism was available. Their compile-time `IS_ENABLED()` line is unaffected;
use `lsmod`/`/proc/modules` to check whether the optional module is loaded.

## Build

```sh
make -C /path/to/kernel/build M="$PWD" modules   # produces shadow_ctr_checker.ko
# or
make KDIR=/path/to/kernel/build
# or, overriding the default sibling-directory lookup:
make KDIR=/path/to/kernel/build \
     KBUILD_EXTRA_SYMBOLS="/abs/path/to/shadow_ns_base/Module.symvers"
```

Build `shadow_ns_base` first — this module needs its `Module.symvers` to
resolve `shadow_ns_base_type_loaded`/`shadow_ns_base_type_real` at modpost
time, and needs `shadow_ns_base.ko` loaded first at runtime for the same
reason. Build inside the matching
`ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image so its `IS_ENABLED()`
checks reflect the real target kernel config.
