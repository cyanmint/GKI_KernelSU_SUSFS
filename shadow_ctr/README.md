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
| `shadow_ns_base`      | `shadow_ns_base/`    | —                       | —                 | Generic shadow-namespace registry + `unshare/setns/clone/fork` hooks + plugin API. Bookkeeping for all 7 types works even with no submodules loaded. |
| `shadow_ns_uts`       | `shadow_ns_uts/`     | —                       | `shadow_ns_base`  | **Real** per-namespace UTS `nodename`/`domainname` + `sethostname`/`setdomainname`/`uname` hooks. |
| `shadow_ns_net`       | `shadow_ns_net/`     | —                       | `shadow_ns_base`  | Thin presence/extension slot for NET (bookkeeping only). |
| `shadow_ns_ipc`       | `shadow_ns_ipc/`     | —                       | `shadow_ns_base`  | Thin presence/extension slot for IPC (bookkeeping only). |
| `shadow_ns_pid`       | `shadow_ns_pid/`     | —                       | `shadow_ns_base`  | Thin presence/extension slot for PID (bookkeeping only). |
| `shadow_ns_mnt`       | `shadow_ns_mnt/`     | —                       | `shadow_ns_base`  | Thin presence/extension slot for MNT (bookkeeping only). |
| `shadow_ns_user`      | `shadow_ns_user/`    | —                       | `shadow_ns_base`  | Thin presence/extension slot for USER (bookkeeping only). |
| `shadow_ns_cgroup`    | `shadow_ns_cgroup/`  | —                       | `shadow_ns_base`  | Thin presence/extension slot for CGROUP (bookkeeping only). |
| `shadow_sysvipc`      | `shadow_sysvipc/`    | —                       | —                 | Simulated System V IPC (msg/sem/shm) via transparent syscall hooks. |
| `shadow_mqueue`       | `shadow_mqueue/`     | —                       | —                 | Simulated POSIX mqueue via transparent syscall hooks + a `"mqueue"` filesystem type. |
| `shadow_cgdevices`    | `shadow_cgdevices/`  | —                       | —                 | Transparent device-open hook shim for the cgroup-device compatibility slot. |
| `shadow_overlay2`     | `shadow_overlay2/`   | (registers `overlay` fs)| —                 | **Real** vendored `fs/overlayfs`. **android14-6.1 only.** |
| `shadow_ctr_checker`  | `shadow_ctr_checker/`| `/dev/shadow_ctr_checker` | — (runtime-optional) | Diagnostics: `cat /dev/shadow_ctr_checker` reports what's supported/hijacked. |

Shared, header-only helpers live in `common/`:

| File                          | Purpose |
|-------------------------------|---------|
| `common/shadow_hook.h`        | ftrace/kprobe syscall-hijack helper used by every hooking module. |
| `common/shadow_ns_base.h`     | The `shadow_ns_base` plugin API contract (used by base, all `shadow_ns_*`, and the checker). |
| `common/shadow_ctr_compat.h`  | `fd_file()`/`fd_empty()` compat shims for kernels < 6.8 (used by `shadow_mqueue`). |
| `common/shadow_hook.README.md`| Documentation for the hook helper. |

## Dependencies & load order

Only the `shadow_ns_*` per-type modules depend on another module
(`shadow_ns_base`); everything else is fully standalone.

```sh
# namespaces (load base first, then whichever types you want):
insmod shadow_ns_base/shadow_ns_base.ko
insmod shadow_ns_uts/shadow_ns_uts.ko        # real UTS
insmod shadow_ns_net/shadow_ns_net.ko        # optional presence markers
# ... shadow_ns_{ipc,pid,mnt,user,cgroup}.ko as desired

# standalone subsystems (any order, independent):
insmod shadow_sysvipc/shadow_sysvipc.ko
insmod shadow_mqueue/shadow_mqueue.ko
insmod shadow_cgdevices/shadow_cgdevices.ko
insmod shadow_overlay2/shadow_overlay2.ko    # android14-6.1 only

# diagnostics (load last so it sees everything):
insmod shadow_ctr_checker/shadow_ctr_checker.ko
cat /dev/shadow_ctr_checker
```

At build time the `shadow_ns_*` submodules resolve `shadow_ns_base`'s exported
symbols via `KBUILD_EXTRA_SYMBOLS`, which each submodule's `Makefile` defaults
to the sibling `../shadow_ns_base/Module.symvers`. **Build `shadow_ns_base`
first.** `shadow_ctr_checker` has no build-time dependency on anything — it
detects the other modules purely at runtime via `symbol_get()`.

## Building

Each directory is a self-contained dual-purpose kbuild module (works both
out-of-tree via `make KDIR=...` and embedded in an in-tree
`obj-$(CONFIG_...)` build). Out-of-tree, against a prepared kernel build tree:

```sh
# base first (produces Module.symvers the ns_* modules need):
make -C /path/to/kernel/build M="$PWD/shadow_ns_base" modules

# a dependent ns module (Makefile auto-points KBUILD_EXTRA_SYMBOLS at base):
make -C /path/to/kernel/build M="$PWD/shadow_ns_uts" \
     KBUILD_EXTRA_SYMBOLS="$PWD/shadow_ns_base/Module.symvers" modules

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
only UTS (`shadow_ns_uts`) and overlayfs (`shadow_overlay2`) provide genuine
functional behaviour; the other `shadow_ns_*` types are reference-counted
bookkeeping only.
