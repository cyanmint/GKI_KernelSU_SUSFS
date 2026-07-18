# shadow_ns_ipc

`shadow_ns_ipc.ko` is a **thin IPC namespace presence/extension
module** for `shadow_ns_base.ko`.

## What it is — and is not

It is **honestly bookkeeping-only**. Loading it does **not** add any real
IPC namespace isolation. All the actual shadow-namespace machinery — the
generic registry, refcounting, and the `unshare`/`setns`/`clone`/`clone3`/
`fork`/`vfork` hooks that create and join shadow namespaces of *every* type —
lives entirely in `shadow_ns_base.ko` and works whether or not this module is
loaded. For example, `unshare(CLONE_NEWNET)` is shadowed by `shadow_ns_base`
alone; `shadow_ns_net.ko` need never be loaded for that bookkeeping to happen.

This module's purpose is twofold:

1. **Explicit enablement.** Loading it is a deliberate statement that the
   deployer has chosen to "enable" the IPC (`SHADOW_NS_TYPE_IPC`) shadow
   namespace type. It is easy to see with `lsmod` which types were switched on.
2. **Extension point.** It claims the per-type plugin slot for `SHADOW_NS_TYPE_IPC`,
   giving a future contributor a place to add genuine per-type behaviour (a
   payload via `priv_alloc`/`priv_free`, and/or custom ioctls via the plugin
   `->ioctl` hook) **without modifying `shadow_ns_base` again** — exactly as
   `shadow_ns_uts.ko` already does for the UTS type.

It registers its ops vector with `.real_support = false` and no payload or
ioctl callbacks, i.e. precisely the refcounted-bookkeeping fidelity that this
namespace type has always had.

## Dependency

Requires `shadow_ns_base.ko` loaded first:

```
insmod shadow_ns_base.ko
insmod shadow_ns_ipc.ko
```

Its `Makefile` wires `KBUILD_EXTRA_SYMBOLS` to the sibling
`shadow_ns_base/Module.symvers` so `shadow_ns_base`'s exported symbols resolve
at build time.

## Build

```sh
# after building shadow_ns_base (which produces its Module.symvers):
make -C /path/to/kernel/build M="$PWD" \
     KBUILD_EXTRA_SYMBOLS="$PWD/../shadow_ns_base/Module.symvers" modules

# or, from this directory:
make KDIR=/path/to/kernel/build
```

Build inside the matching `ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image,
not a bare `gki_defconfig` tree.
