# shadow_ns_uts

`shadow_ns_uts.ko` is the **UTS namespace extension** for `shadow_ns_base.ko`.

Of the seven shadow namespace types managed by `shadow_ns_base`, UTS is the
only one that gets **genuine functional behaviour** rather than mere
reference-counted bookkeeping: this module gives every shadow UTS namespace its
own real `nodename` (hostname) and `domainname` storage, and makes an
unmodified process transparently observe/modify *its shadow namespace's*
hostname instead of the host's.

## Dependency

This module **requires `shadow_ns_base.ko` to be loaded first**. It resolves
`shadow_ns_base`'s exported symbols (`shadow_ns_base_register_type`,
`shadow_ns_base_get_current`, `shadow_ns_base_put`, `shadow_ns_base_priv`, …)
at load time. Its `Makefile` wires `KBUILD_EXTRA_SYMBOLS` to the sibling
`shadow_ns_base/Module.symvers` so modpost resolves these symbols at build
time with no "undefined symbol" warnings.

```
insmod shadow_ns_base.ko
insmod shadow_ns_uts.ko
```

## What it does

- Registers the `SHADOW_NS_TYPE_UTS` type with `shadow_ns_base` via the plugin
  API (`common/shadow_ns_base.h`), with `.real_support = true`.
- Attaches a per-namespace payload (`struct shadow_uts_priv`: a mutex plus
  `nodename`/`domainname` buffers) to every shadow UTS namespace through the
  `priv_alloc`/`priv_free` callbacks. A child UTS namespace inherits its
  parent's names at creation time.
- Installs transparent hooks (via `../common/shadow_hook.h`, the same
  ftrace/kprobe hijack helper used across the project) on the
  `sethostname(2)`, `setdomainname(2)` and `newuname(2)`/`uname(2)` syscalls:
  - `sethostname`/`setdomainname` update the caller's shadow UTS namespace
    names (requiring `CAP_SYS_ADMIN`) when it has one; otherwise they fall
    through to the real syscall.
  - `uname` runs the real syscall, then overwrites the returned
    `nodename`/`domainname` with the shadow namespace's values when the caller
    has a shadow UTS namespace.

These hooks are installed by **this** module (not by `shadow_ns_base`), so
loading `shadow_ns_base` alone never touches the hostname syscalls; UTS
shadowing is entirely opt-in by loading `shadow_ns_uts`.

## Scope / honesty

This module does **not** create a real kernel `struct uts_namespace`. It is a
user-space-visible simulation: it shadows what `gethostname`/`uname`/etc.
report for tasks that have joined a shadow UTS namespace. That is exactly the
fidelity level documented for UTS in the original combined module; only the
packaging changed (it is now an independently loadable `.ko`).

## Build

```sh
# Build shadow_ns_base first (produces its Module.symvers):
make -C /path/to/kernel/build M="$PWD/../shadow_ns_base" modules

# Then build this module, pointing at that Module.symvers:
make -C /path/to/kernel/build M="$PWD" \
     KBUILD_EXTRA_SYMBOLS="$PWD/../shadow_ns_base/Module.symvers" modules

# Or, from this directory, using the convenience wrapper (auto-detects the
# sibling shadow_ns_base/Module.symvers):
make KDIR=/path/to/kernel/build
```

Out-of-tree modules for GKI **must** be built inside the matching
`ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image against its real
`vmlinux`/`Module.symvers`, not a bare `gki_defconfig` tree, or `insmod` will
panic on the real kernel.
