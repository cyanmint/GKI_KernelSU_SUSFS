# shadow_ns_base — generic shadow-namespace registry + hook module

`shadow_ns_base.ko` is the core of the shadow-namespace family. It is a
**standalone, independently loadable** module that owns:

* the generic, reference-counted **registry** of *shadow* namespace objects
  (a global `xarray` of `struct shadow_ns`, one per created namespace, keyed by
  a stable integer id), for all seven namespace types
  (`UTS/IPC/MNT/PID/NET/USER/CGROUP`);
* the transparent **syscall-hijack path** (built on `../common/shadow_hook.h`)
  that intercepts `unshare(2)`, `setns(2)`, `clone(2)`/`clone3(2)`/`fork(2)`/
  `vfork(2)` so a stock, unmodified `containerd`/`runc`/`dockerd` reaches the
  shadow implementation via the real syscalls;
* a small **plugin API** (`../common/shadow_ns_base.h`) that lets optional
  per-type submodules add behaviour on top of the generic bookkeeping.

The generic bookkeeping works for **every** namespace type regardless of which
(if any) per-type submodule is loaded. For example `unshare(CLONE_NEWNET)` is
shadowed by `shadow_ns_base` alone; `shadow_ns_net.ko` need never be loaded.

## Split from the old combined module

This module was previously the `shadow_ns.c` translation unit inside the
combined `shadow_ctr.ko`. In the new layout:

* **UTS-specific behaviour moved out** of the generic path into
  [`../shadow_ns_uts`](../shadow_ns_uts) (real `nodename`/`domainname` storage
  plus the `sethostname`/`setdomainname`/`uname` hooks).
  `shadow_ns_base` itself no longer contains any UTS payload.
* Each generic `struct shadow_ns` now carries an opaque `void *type_priv`
  instead of an inline UTS struct; per-type submodules attach/detach their own
  payload through the plugin API.
* `shadow_ns_base` has its own `module_init`/`module_exit` and `MODULE_*`
  metadata (there is exactly one `init_module`/`cleanup_module` alias per
  `.ko`).

## Plugin API (`common/shadow_ns_base.h`)

Per-type submodules register with:

```c
int  shadow_ns_base_register_type(u32 type, const struct shadow_ns_type_ops *ops);
void shadow_ns_base_unregister_type(u32 type);
```

`struct shadow_ns_type_ops` provides these optional extension points:

| Field          | Purpose                                                        |
|----------------|----------------------------------------------------------------|
| `owner`        | `THIS_MODULE` of the submodule (see module-ref note below).    |
| `priv_alloc`   | Allocate a per-namespace payload when a namespace of this type is created (inherits from parent). |
| `priv_free`    | Free that payload just before the `shadow_ns` object is freed. |
| `real_support` | `true` if this type gets genuine functional behaviour beyond bookkeeping (only UTS today). |

Introspection helpers for consumers such as `shadow_ctr_checker`:

```c
bool shadow_ns_base_type_loaded(u32 type); /* a submodule registered for type? */
bool shadow_ns_base_type_real(u32 type);   /* is that submodule real_support?   */
```

Narrow accessors for submodules that need the current task's shadow namespace
(the `struct shadow_ns` layout stays private to `shadow_ns_base.c`; submodules
only ever see an opaque pointer):

```c
struct shadow_ns *shadow_ns_base_get_current(u32 type); /* takes a ref */
void              shadow_ns_base_put(struct shadow_ns *ns);
void             *shadow_ns_base_priv(struct shadow_ns *ns); /* the type_priv */
```

All of the above are `EXPORT_SYMBOL_GPL()` (these are GPL-2.0 modules).

### Module-reference model (design note / deviation)

The plugin API pins a submodule's `owner` with `try_module_get()` **only for
the duration of each callback** (`priv_alloc`/`priv_free`), releasing
it immediately after. It does **not** hold a persistent reference for the whole
registration lifetime.

This differs from a naive "pin on register, unpin on unregister" scheme: since
a submodule's `unregister` runs from its own `module_exit`, and `module_exit`
only runs at refcount 0, a persistent base-held reference would make the
submodule permanently un-`rmmod`-able (a self-deadlock). The per-call pin still
guarantees a submodule's code cannot be freed mid-callback, and
`shadow_ns_base` itself cannot be unloaded while any submodule is loaded because
the submodule links against `shadow_ns_base`'s exported symbols (the normal
kernel module-dependency mechanism). Net effect: base and submodules are each
independently loadable/unloadable, safely.

## What is actually simulated

| Type    | Behaviour                                                                | Provided by            |
|---------|--------------------------------------------------------------------------|------------------------|
| UTS     | **Functional** per-namespace `nodename`/`domainname`.                    | `shadow_ns_uts.ko`     |
| IPC     | Reference-counted membership + parent lineage (bookkeeping only).        | base (+`shadow_ns_ipc`)|
| MNT     | Reference-counted membership + parent lineage (bookkeeping only).        | base (+`shadow_ns_mnt`)|
| PID     | Reference-counted membership + parent lineage (bookkeeping only).        | base (+`shadow_ns_pid`)|
| NET     | Reference-counted membership + parent lineage (bookkeeping only).        | base (+`shadow_ns_net`)|
| USER    | Reference-counted membership + parent lineage (bookkeeping only).        | base (+`shadow_ns_user`)|
| CGROUP  | Reference-counted membership + parent lineage (bookkeeping only).        | base (+`shadow_ns_cgroup`)|

The thin `shadow_ns_<type>.ko` submodules do **not** add isolation; they mark a
type as intentionally enabled and reserve its plugin slot for future real
support. See each submodule's README.

## Honest scope / limitations

`shadow_ns_base` remains a **simulation and bookkeeping layer**, not a
replacement for real kernel isolation. The native namespace machinery is
compiled into `vmlinux` (it adds fields to `task_struct`/`nsproxy`/`cred`), so a
module loaded afterwards cannot add *real* namespaces to a running kernel.

* Only UTS (via `shadow_ns_uts.ko`) is functionally real; all other types are
  bookkeeping-only, exactly as before the split.
* Transparent `setns(2)` is intentionally partial: shadow-only joins use the
  numeric shadow namespace id fallback, not synthesized nsfs fds for
  `/proc/<pid>/ns/*`.
* Child shadow-state installation after fork-like syscalls is best-effort.
* Dead-TGID state is reaped periodically (best-effort), not synchronously at
  task exit.

## Files

| Path                       | Purpose                                                        |
|----------------------------|----------------------------------------------------------------|
| `include/uapi/shadow_ns.h` | Shared namespace-type constants + UTS payload layout.          |
| `shadow_ns_base.c`         | Registry, transparent hooks, numeric-id `setns` fallback, plugin API. |
| `Makefile`                 | Out-of-tree build (`make KDIR=...`).                           |

## Building

```sh
cd shadow_ctr/shadow_ns_base
make KDIR=/path/to/kernel/build       # produces shadow_ns_base.ko + Module.symvers
sudo insmod shadow_ns_base.ko
```

Android GKI cross build:

```sh
make -C /path/to/kernel/build M=$(pwd) ARCH=arm64 LLVM=1 modules
```

Build inside the matching `ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image.
The generated `Module.symvers` is what the per-type submodules consume via
`KBUILD_EXTRA_SYMBOLS`, so build `shadow_ns_base` **first**.

## Loading

```sh
insmod shadow_ns_base.ko           # generic bookkeeping + transparent hooks for all types
insmod shadow_ns_uts.ko            # optional: real UTS support
insmod shadow_ns_net.ko            # optional: mark NET enabled (bookkeeping)
# ...
```
