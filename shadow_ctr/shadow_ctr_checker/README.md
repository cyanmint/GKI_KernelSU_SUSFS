# shadow_ctr_checker

`shadow_ctr_checker.ko` is a **standalone diagnostics module** for the
`shadow_ctr` family. It registers a read-only character device,
`/dev/shadow_ctr_checker` (mode `0444`); reading it produces a plain,
greppable, one-line-per-check report of which container-relevant kernel
features are available and, for each, whether that support is native to the
kernel or provided by a loaded `shadow_*` module.

```sh
insmod shadow_ctr_checker.ko
cat /dev/shadow_ctr_checker
```

Example output:

```
shadow_ctr_checker v2.0 - shadow container-support status
----------------------------------------
mqueue: supported (shadow_mqueue.ko)
sysvipc: not supported
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
| `mqueue`            | `IS_ENABLED(CONFIG_POSIX_MQUEUE)`    | `shadow_mqueue.ko` loaded |
| `sysvipc`           | `IS_ENABLED(CONFIG_SYSVIPC)`         | `shadow_sysvipc.ko` loaded |
| `cgroup_device`     | `IS_ENABLED(CONFIG_CGROUP_DEVICE)`   | `shadow_cgdevices.ko` loaded |
| `overlay2`          | `get_fs_type("overlay")` ground truth| distinguishes builtin / `shadow_overlay2.ko` / other module |
| `ns_net`            | `IS_ENABLED(CONFIG_NET_NS)`          | `shadow_ns_base` reports NET loaded/real |
| `ns_pid`            | `IS_ENABLED(CONFIG_PID_NS)`          | … PID |
| `ns_ipc`            | `IS_ENABLED(CONFIG_IPC_NS)`          | … IPC |
| `ns_uts`            | `IS_ENABLED(CONFIG_UTS_NS)`          | … UTS |
| `ns_mnt`            | `IS_ENABLED(CONFIG_MNT_NS)`          | … MNT |
| `ns_user`           | `IS_ENABLED(CONFIG_USER_NS)`         | … USER (called out explicitly) |
| `ns_cgroup`         | `IS_ENABLED(CONFIG_CGROUPS)` (proxy) | … CGROUP |

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

## Optional-dependency technique (no Kbuild dependency edge)

The checker's whole purpose is to work **standalone**, regardless of which
subset of the other `shadow_*` modules is loaded. It therefore has **no**
build-time dependency (`KBUILD_EXTRA_SYMBOLS` / `Module.symvers`) on any other
module — its `Makefile` only adds a header include path for the
`SHADOW_NS_TYPE_*` enum. If it linked against another module's symbols,
`insmod` of the checker would fail with "unknown symbol" whenever that module
was absent.

Instead it uses the kernel's standard runtime symbol-resolution primitive,
`symbol_get()` / `symbol_put()` (backed by `__symbol_get()`), **uniformly** for
every shadow module it queries:

- each queryable module exports a tiny presence marker —
  `shadow_mqueue_is_active`, `shadow_sysvipc_is_active`,
  `shadow_cgdevices_is_active`, `shadow_overlay2_is_active`, and
  `shadow_ns_base`'s `shadow_ns_base_type_loaded` / `shadow_ns_base_type_real`;
- `symbol_get("...")` returns non-`NULL` only when the providing module is
  currently loaded, and pins it for the duration of the query (released
  immediately with `symbol_put()`), so a missing module is handled gracefully
  as "not loaded".

This is deliberately chosen over a `find_module()`-based scan so the same
mechanism (and the same "pin while querying" safety) applies to every check,
including the `shadow_ns_base` introspection calls.

## Build

```sh
make -C /path/to/kernel/build M="$PWD" modules   # produces shadow_ctr_checker.ko
# or
make KDIR=/path/to/kernel/build
```

No sibling module's `Module.symvers` is required to build or load it. Build
inside the matching `ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image so its
`IS_ENABLED()` checks reflect the real target kernel config.
