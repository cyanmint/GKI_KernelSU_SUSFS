# shadow_ctr — container-support kernel modules for GKI

`shadow_ctr/` now builds a single out-of-tree kernel module,
**`shadow_ctr.ko`**, that makes a stock, unpatched
`containerd`/`runc`/`dockerd` run on Android GKI kernels that were built
without the usual container prerequisites (`CONFIG_*_NS`, `CONFIG_SYSVIPC`,
`CONFIG_POSIX_MQUEUE`, cgroup-v1 device control, …).

The source remains split by subsystem under `shadow_ctr/shadow_ctr/` for
maintainability, but build/load/deploy is unified again: one Makefile, one
module entry point, one `.ko`.

## Module map

| Component             | Directory                         | `/dev` node | Summary |
|----------------------|-----------------------------------|-------------|---------|
| `shadow_ctr.ko`      | `shadow_ctr/`                     | —           | Unified module containing the shared hook engine plus the namespace, SysV IPC, POSIX mqueue and cgroup-device compatibility subsystems. |
| `shadow_hijack`      | `shadow_ctr/shadow_hijack/`       | —           | Internal shared ftrace/kprobe hook implementation used by the other subsystems inside `shadow_ctr.ko`. |
| `shadow_ns`          | `shadow_ctr/shadow_ns/`           | —           | `unshare/setns/clone/clone3/fork/vfork` hooks. Real per-namespace isolation for UTS, PID and USER; bookkeeping only for IPC/NET/CGROUP/MNT when genuinely absent. |
| `shadow_sysvipc`     | `shadow_ctr/shadow_sysvipc/`      | —           | System V IPC (msg/sem/shm) hooks and shadow registry. |
| `shadow_mqueue`      | `shadow_ctr/shadow_mqueue/`       | —           | POSIX mqueue hooks plus real shadow message transfer. |
| `shadow_cgdevices`   | `shadow_ctr/shadow_cgdevices/`    | —           | Transparent device-open hook shim for the cgroup-device compatibility slot. |
| `shadow_ctr_checker` | `shadow_ctr_checker/`             | n/a         | **Userspace** diagnostic binary (not a kernel module): performs real syscalls and reports PASS/STUB/FAIL per feature. |

### Real vs. bookkeeping vs. stub — a quick reference

Because every subsystem in this family hooks/simulates functionality that would
normally be compiled into `vmlinux`, "supported" does not always mean the same
thing. This table is the single place that spells out, per subsystem (and per
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
| `shadow_ctr_checker`                  | n/a (diagnostics) | **Userspace binary**, not a kernel module: actually attempts the relevant syscalls and reports PASS/STUB/FAIL based on the observed effect, rather than reporting compile-time config alone. |

Shared, header-only helpers live in `common/`:

| File                          | Purpose |
|-------------------------------|---------|
| `common/shadow_hook.h`        | ftrace/kprobe syscall-hijack helper used by every hooking subsystem. |
| `common/shadow_ctr_compat.h`  | `fd_file()`/`fd_empty()` compat shims for kernels < 6.8 (used by `shadow_mqueue`). |
| `common/shadow_hook.README.md`| Documentation for the hook helper. |

## Load order

There is now exactly one kernel module to load:

```sh
insmod shadow_ctr/shadow_ctr/shadow_ctr.ko

# diagnostics: a plain userspace binary, run any time, no insmod needed:
./shadow_ctr_checker/shadow_ctr_checker
```

`shadow_ctr_checker` is a **userspace** diagnostic program, not a kernel
module: it has no build-time or load-time dependency on `shadow_ctr.ko`, and
instead of reporting
compile-time `IS_ENABLED(CONFIG_*)` facts, it directly performs the relevant
syscalls (`unshare`/`fork`/`setns`, `mq_*`, `msg*`, `mount`) and reports
PASS/STUB/FAIL based on their actual observed effect. See
`shadow_ctr_checker/README.md` for the full methodology and why this
replaced the earlier kernel-module version.

## Building

The merged kernel module lives in `shadow_ctr/shadow_ctr/` as a dual-purpose
kbuild module (works both out-of-tree via `make KDIR=...` and embedded in an
in-tree `obj-$(CONFIG_...)` build). Out-of-tree, against a prepared kernel
build tree:

```sh
make -C /path/to/kernel/build M="$PWD/shadow_ctr/shadow_ctr" modules
```

`shadow_ctr_checker` is a plain userspace program and builds with a normal C
compiler — no `KDIR`/kernel build tree involved:

```sh
make -C shadow_ctr_checker                          # host toolchain
make -C shadow_ctr_checker CC="clang --target=aarch64-linux-gnu"  # cross build
```

Out-of-tree modules for GKI **must** be built inside the matching
`ghcr.io/ylarod/ddk-min:<kmi>-<release>` DDK image against its real
`vmlinux`/`Module.symvers`, not a bare `gki_defconfig` tree, or `insmod` will
panic on the real kernel. See
[`../.github/workflows/build-shadow-ctr.yml`](../.github/workflows/build-shadow-ctr.yml)
(merged-module build matrix) and
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

See each subsystem's own README for its honest scope/limitations. In
particular, `shadow_ns` only ever simulates a namespace type genuinely absent
from this
kernel build (`IS_ENABLED(CONFIG_*_NS)`, which collapses correctly even when
`CONFIG_NAMESPACES` is disabled entirely) — when it does simulate, UTS/PID/USER
get real functional isolation; IPC/NET simulation (when needed) remains
reference-counted bookkeeping only. See
[`shadow_ctr/shadow_ns/README.md`](shadow_ctr/shadow_ns/README.md) for the full
design rationale, and the "Real vs. bookkeeping vs. stub" table above for the
full picture across every subsystem in this family.
