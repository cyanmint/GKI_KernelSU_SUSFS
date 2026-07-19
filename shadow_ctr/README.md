# shadow_ctr — container-support kernel modules for GKI

`shadow_ctr/` is a family of **independently loadable** out-of-tree kernel
modules that make a stock, unpatched `containerd`/`runc`/`dockerd` run on
Android GKI kernels that were built without the usual container prerequisites
(`CONFIG_*_NS`, `CONFIG_SYSVIPC`, `CONFIG_POSIX_MQUEUE`, cgroup-v1 device
control, `CONFIG_OVERLAY_FS`, …).

Previously these were a single combined `shadow_ctr.ko`. They are now split so
each subsystem is its own `.ko` that can be loaded (and unloaded) on its own,
and so a deployer can ship exactly the subset a given kernel needs.

## Module map

| Module                | Directory            | `/dev` node             | Depends on        | Summary |
|-----------------------|----------------------|-------------------------|-------------------|---------|
| `shadow_ns`           | `shadow_ns/`         | —                       | —                 | Single, standalone namespace system: `unshare/setns/clone/clone3/fork/vfork` hooks. Real per-namespace isolation for UTS (nodename/domainname), PID (vpid↔rpid remapping) and USER (uid/gid=0 remapping); bookkeeping only for IPC/NET (MNT/CGROUP are always builtin). |
| `shadow_sysvipc`      | `shadow_sysvipc/`    | —                       | —                 | Simulated System V IPC (msg/sem/shm) via transparent syscall hooks. |
| `shadow_mqueue`       | `shadow_mqueue/`     | —                       | —                 | Simulated POSIX mqueue via transparent syscall hooks. |
| `shadow_cgdevices`    | `shadow_cgdevices/`  | —                       | —                 | Transparent device-open hook shim for the cgroup-device compatibility slot. |
| `shadow_overlay2`     | `shadow_overlay2/`   | (registers `overlay` fs)| —                 | **Real** vendored `fs/overlayfs`. **android14-6.1 only.** |
| `shadow_ctr_checker`  | `shadow_ctr_checker/`| `/dev/shadow_ctr_checker` | —                | Diagnostics: `cat /dev/shadow_ctr_checker` reports what's supported/hijacked. Pure standalone tool, no dependency on any other module. |

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
insmod shadow_overlay2/shadow_overlay2.ko    # android14-6.1 only

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

`shadow_overlay2` only compiles against **android14-6.1** (the `fs/overlayfs`
VFS ABI differs on other KMIs), so CI builds it for that KMI only. See
[`shadow_overlay2/README.md`](shadow_overlay2/README.md).

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
get real functional isolation and overlayfs (`shadow_overlay2`) is a real
vendored filesystem; IPC/NET simulation (when needed) remains reference-counted
bookkeeping only. See [`shadow_ns/README.md`](shadow_ns/README.md) for the full
design rationale.
