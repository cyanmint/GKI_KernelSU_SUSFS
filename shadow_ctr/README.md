# shadow_ctr — container-support kernel modules for GKI

`shadow_ctr/` is a family of **independently loadable** out-of-tree kernel
modules that make a stock, unpatched `containerd`/`runc`/`dockerd` run on
Android GKI kernels that were built without the usual container prerequisites
(`CONFIG_*_NS`, `CONFIG_SYSVIPC`, `CONFIG_POSIX_MQUEUE`, cgroup-v1 device
control, …).

Previously these were a single combined `shadow_ctr.ko`. They are now split so
each subsystem is its own `.ko` that can be loaded (and unloaded) on its own,
and so a deployer can ship exactly the subset a given kernel needs.

## Module map

| Module                | Directory            | `/dev` node             | Depends on        | Summary |
|-----------------------|----------------------|-------------------------|-------------------|---------|
| `shadow_ns`           | `shadow_ns/`         | —                       | —                 | Single, standalone namespace system: `unshare/setns/clone/clone3/fork/vfork` hooks. Real per-namespace isolation for UTS (nodename/domainname), PID (vpid↔rpid remapping) and USER (uid/gid=0 remapping); bookkeeping only for IPC/NET/CGROUP/MNT when genuinely absent. |
| `shadow_sysvipc`      | `shadow_sysvipc/`    | —                       | —                 | System V IPC (msg/sem/shm) via transparent syscall hooks: real, functioning object registry with ids/keys/lifecycle — not just a stub returning success. |
| `shadow_mqueue`       | `shadow_mqueue/`     | —                       | —                 | POSIX mqueue via transparent syscall hooks: real message transfer (priority-ordered queue, blocking send/receive with timeouts, real fds) — not just a stub. |
| `shadow_cgdevices`    | `shadow_cgdevices/`  | —                       | —                 | Transparent device-open hook shim for the cgroup-device compatibility slot: currently a pass-through stub that preserves native behaviour rather than enforcing rules. |
| `shadow_ctr_checker`  | `shadow_ctr_checker/`| `/dev/shadow_ctr_checker` | —                | Diagnostics: `cat /dev/shadow_ctr_checker` reports what's supported/hijacked. Pure standalone tool, no dependency on any other module. |

### Real vs. bookkeeping vs. stub — a quick reference

Because every module in this family hooks/simulates functionality that would
normally be compiled into `vmlinux`, "supported" does not always mean the same
thing. This table is the single place that spells out, per module (and per
namespace type inside `shadow_ns`), whether the simulation is **real**
(behaves like the native kernel feature, verified by observable side effects),
**bookkeeping-only** (state is tracked and syscalls succeed, but there is no
functional isolation/enforcement behind it), or a **stub** (a hook exists but
currently only preserves/passes through native behaviour, i.e. it does not yet
change anything). See each module's own README for the full rationale.

| Component                          | Classification | Why |
|-------------------------------------|-----------------|-----|
| `shadow_ns` — UTS namespace          | **Real**        | `uname()`/`sethostname()` after `unshare(CLONE_NEWUTS)` observe a genuinely separate nodename/domainname per simulated namespace. |
| `shadow_ns` — PID namespace          | **Real**        | vpid↔rpid remapping means `getpid()`/`/proc` inside a simulated PID namespace show virtual, namespace-local PIDs distinct from the real ones. |
| `shadow_ns` — USER namespace         | **Real**        | uid/gid 0 remapping gives genuinely different credential mapping inside vs. outside the simulated namespace. |
| `shadow_ns` — IPC namespace          | **Bookkeeping**  | A separate namespace id/refcount is tracked on `unshare`/`setns`/`clone(CLONE_NEWIPC)`, but SysV IPC/mqueue objects are not actually partitioned per namespace — no functional isolation. |
| `shadow_ns` — NET namespace          | **Bookkeeping**  | Same as IPC: id/refcount tracked, but no network-stack partitioning is provided. |
| `shadow_ns` — CGROUP namespace       | **Bookkeeping** (only if `CONFIG_CGROUPS=n`) | Same generic id/refcount registry as IPC/NET, used only on the (rare — no GKI defconfig disables it) kernel builds without `CONFIG_CGROUPS`; otherwise always builtin/passthrough. |
| `shadow_ns` — MNT namespace          | bookkeeping (only if `CONFIG_NAMESPACES=n`, effectively never in practice)   | Mount namespaces have no dedicated per-type Kconfig gate anywhere in mainline Linux, so `shadow_ns` keys MNT's builtin status off `CONFIG_NAMESPACES` itself as a defensive fallback; the real, always-compiled-in mount-namespace code keeps running regardless, this only adds a parallel bookkeeping entry. |
| `shadow_sysvipc` (msg/sem/shm)        | **Real**        | Maintains an actual object registry (ids, keys, lifecycle) behind the hooked syscalls — not a stub that just returns success. |
| `shadow_mqueue` (POSIX mqueue)        | **Real**        | Real message transfer: priority-ordered queue, blocking send/receive with timeout semantics, real anon-inode-backed fds. |
| `shadow_cgdevices` (`chrdev_open`)     | **Stub**        | Hook installed but currently only preserves native behaviour; no rule enforcement yet. |
| `shadow_cgdevices` (`blkdev_open`)     | **Stub, best-effort** | Same as above, and only installed if the symbol exists with the expected prototype on that KMI. |
| `shadow_ctr_checker`                  | n/a (diagnostics) | Read-only reporting tool; does not simulate or hook any container-relevant behaviour itself. |

Shared, header-only helpers live in `common/`:

| File                          | Purpose |
|-------------------------------|---------|
| `common/shadow_hook.h`        | ftrace/kprobe syscall-hijack helper used by every hooking module. |
| `common/shadow_ctr_compat.h`  | `fd_file()`/`fd_empty()` compat shims for kernels < 6.8 (used by `shadow_mqueue`). |
| `common/shadow_hook.README.md`| Documentation for the hook helper. |

## Dependencies & load order

Every module in this family, including `shadow_ns` and `shadow_ctr_checker`,
is fully standalone — none of them has a build-time or load-time dependency
on any other `shadow_ctr` module. (`shadow_ns`/`shadow_sysvipc`/
`shadow_mqueue`/`shadow_cgdevices` do each resolve `shadow_hijack`'s
`EXPORT_SYMBOL_GPL` hook-install/-remove API via `KBUILD_EXTRA_SYMBOLS`, so
`shadow_hijack` should be loaded first for those.) They can therefore be
loaded in any order, and any subset of them can be present.

```sh
insmod shadow_hijack/shadow_hijack.ko        # needed by shadow_ns/sysvipc/mqueue/cgdevices
insmod shadow_ns/shadow_ns.ko                # single namespace system

# standalone subsystems (any order, independent):
insmod shadow_sysvipc/shadow_sysvipc.ko
insmod shadow_mqueue/shadow_mqueue.ko
insmod shadow_cgdevices/shadow_cgdevices.ko

# diagnostics (any order; a pure standalone tool with no dependencies):
insmod shadow_ctr_checker/shadow_ctr_checker.ko
cat /dev/shadow_ctr_checker
```

`shadow_ctr_checker` has no build-time (`KBUILD_EXTRA_SYMBOLS`/
`Module.symvers`) or load-time dependency on any other module: it derives its
namespace-related report lines purely from the same compile-time
`IS_ENABLED(CONFIG_*)` checks `shadow_ns.c` itself uses, instead of calling
into `shadow_ns` at runtime. (An earlier revision took a hard dependency on
`shadow_ns`'s `EXPORT_SYMBOL_GPL` query API, and before that used
`symbol_get()`/`symbol_put()` to detect modules purely at runtime with no
build-time dependency — but `__symbol_get()`/`__symbol_put()` are themselves
trimmed from production GKI kernels' exported-symbol table, which made
`insmod` of the checker fail unconditionally with "Unknown symbol
__symbol_get" / `-ENOENT`. See `shadow_ctr_checker/README.md` for details.)

## Building

Each directory is a self-contained dual-purpose kbuild module (works both
out-of-tree via `make KDIR=...` and embedded in an in-tree
`obj-$(CONFIG_...)` build). Out-of-tree, against a prepared kernel build tree:

```sh
# shadow_ns:
make -C /path/to/kernel/build M="$PWD/shadow_ns" modules

# the checker (no dependencies at all):
make -C /path/to/kernel/build M="$PWD/shadow_ctr_checker" modules

# a standalone module:
make -C /path/to/kernel/build M="$PWD/shadow_sysvipc" modules
```

Out-of-tree modules for GKI **must** be built inside the matching
`ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image against its real
`vmlinux`/`Module.symvers`, not a bare `gki_defconfig` tree, or `insmod` will
panic on the real kernel. See
[`../.github/workflows/build-shadow-ctr.yml`](../.github/workflows/build-shadow-ctr.yml)
(multi-module build matrix) and
[`../.github/workflows/test-shadow-ctr-qemu.yml`](../.github/workflows/test-shadow-ctr-qemu.yml)
(QEMU boot/load test).

## Kernel compatibility

`common/shadow_hook.h` picks its hooking backend at compile time: ftrace-based
when `CONFIG_FUNCTION_TRACER`/`CONFIG_DYNAMIC_FTRACE` are available, and a
kprobe-`pre_handler` fallback otherwise — the latter is what runs on stock
Android GKI kernels, which ship with `CONFIG_FUNCTION_TRACER` disabled.

`shadow_mqueue`'s `fd_file()`/`fd_empty()` use targets a kernel API that only
exists from Linux v6.8 onward; `common/shadow_ctr_compat.h` provides shims so
the same source builds unmodified against older GKI branches (e.g. 6.1).

See each module's own README for its honest scope/limitations. In particular,
`shadow_ns` only ever simulates a namespace type genuinely absent from this
kernel build (`IS_ENABLED(CONFIG_*_NS)`, which collapses correctly even when
`CONFIG_NAMESPACES` is disabled entirely) — when it does simulate, UTS/PID/USER
get real functional isolation; IPC/NET simulation (when needed) remains
reference-counted bookkeeping only. See [`shadow_ns/README.md`](shadow_ns/README.md)
for the full design rationale, and the "Real vs. bookkeeping vs. stub" table
above for the full picture across every module in this family.
