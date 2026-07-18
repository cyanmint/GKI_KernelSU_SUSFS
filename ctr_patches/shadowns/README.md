# shadowns — simulated ("shadow") namespace subsystem

`shadowns` is a **standalone loadable kernel module** that provides an
independent, reference-counted set of *shadow* namespace objects, driven
entirely from userspace through ioctls on `/dev/shadowns`. A **patched
containerd/runc** can talk to this ABI to obtain namespace-like semantics on a
GKI kernel that was built **without** the native `CONFIG_*_NS` options.

## Why this exists

The native Linux namespace machinery (`CONFIG_UTS_NS`, `CONFIG_IPC_NS`,
`CONFIG_PID_NS`, `CONFIG_NET_NS`, `CONFIG_USER_NS`, …) is compiled **directly
into `vmlinux`**. It adds fields to `task_struct`, `nsproxy` and `cred`, and
wires `unshare(2)`/`setns(2)`/`clone(2)` into the core kernel. Those struct
layouts and the syscall table are frozen when the kernel image is built, so a
module loaded afterwards **cannot** add real namespaces to a running kernel.

`shadowns` does not attempt the impossible. Instead of hooking the native code
paths, it maintains a **parallel** namespace model that a patched runtime opts
into explicitly. This keeps the module 100% out-of-tree and ABI-safe.

## Architecture

```
  patched containerd / runc                 shadowns.ko
  ┌───────────────────────┐   ioctl   ┌────────────────────────────┐
  │ shadowns.go client    │──────────▶│ /dev/shadowns misc device  │
  │  Open()               │           │  per-fd "session"          │
  │  Unshare(TypeUTS)     │           │   cur[TYPE] -> shadow_ns    │
  │  SetHostname(...)     │           │  global id -> shadow_ns    │
  │  SetNS(id)            │           │   (refcounted, xarray)     │
  └───────────────────────┘           └────────────────────────────┘
```

* **Session = open fd.** Each `open("/dev/shadowns")` is one session (think: one
  container bring-up). All references a session holds are dropped automatically
  on `close()`/process exit, so a crashed runtime never leaks state.
* **Shadow namespaces are reference counted** and live in a global `xarray`
  keyed by a stable integer id. Sessions share namespaces by id: one session
  creates an id, another can `SetNS` to it (join).
* **Verbs mirror the native ones:** `CREATE` (detached), `UNSHARE`
  (create-derived-and-join), `SETNS` (join by id), `GET` (query current id),
  `DESTROY` (drop reference).

## What is actually simulated

| Type    | Behaviour provided by shadowns                                            |
|---------|---------------------------------------------------------------------------|
| UTS     | **Functional** per-namespace `nodename`/`domainname` storage (get/set).   |
| IPC     | Reference-counted membership + parent lineage (bookkeeping only).          |
| MNT     | Reference-counted membership + parent lineage (bookkeeping only).          |
| PID     | Reference-counted membership + parent lineage (bookkeeping only).          |
| NET     | Reference-counted membership + parent lineage (bookkeeping only).          |
| USER    | Reference-counted membership + parent lineage (bookkeeping only).          |
| CGROUP  | Reference-counted membership + parent lineage (bookkeeping only).          |

### Honest scope / limitations

`shadowns` is a **simulation and bookkeeping layer**, not a replacement for real
kernel isolation:

* UTS is genuinely isolated: a runtime can give each container its own hostname
  and read it back, independent of the host.
* IPC/MNT/PID/NET/USER/CGROUP provide **stable namespace identities and join
  semantics**, but they do **not** enforce real kernel-level isolation of those
  subsystems. Real isolation for those can only come from the native,
  compiled-in namespaces. Treat these as identity/lifecycle tracking that lets a
  patched runtime proceed instead of failing when the native namespace is
  absent.
* This module is intended for experimentation and for runtimes that have been
  explicitly patched to understand the shadow model. It is **not** a drop-in
  that makes an unmodified containerd fully isolate containers.

## Files

| Path                              | Purpose                                        |
|-----------------------------------|------------------------------------------------|
| `include/uapi/shadowns.h`         | Stable ioctl ABI shared with userspace.        |
| `shadowns.c`                      | The kernel module.                             |
| `Makefile`                        | Out-of-tree build (`make KDIR=...`).           |
| `Kbuild`                          | In-tree build fragment (`CONFIG_SHADOWNS`).    |
| `containerd/shadowns.go`          | Reference Go client to vendor into containerd. |
| `containerd/README.md`            | Runtime integration notes.                     |

## Building

### Out-of-tree (produces `shadowns.ko`)

```sh
cd ctr_patches/shadowns
make KDIR=/path/to/kernel/build          # e.g. .../out or /lib/modules/$(uname -r)/build
sudo insmod shadowns.ko
ls -l /dev/shadowns
```

For an Android GKI cross build, point `KDIR` at the prepared kernel `out`
directory and pass the same `ARCH=arm64 LLVM=1` flags used to build the kernel:

```sh
make -C /path/to/kernel/build M=$(pwd) ARCH=arm64 LLVM=1 modules
```

### In-tree

Copy this directory into the kernel source (e.g. `drivers/shadowns/`), add a
`CONFIG_SHADOWNS` Kconfig entry, and select it, or have the build inject
`obj-m += shadowns.o` the same way this repo injects other out-of-tree sources.

## Loading

```sh
insmod shadowns.ko          # creates /dev/shadowns (mode 0600, root only)
dmesg | grep shadowns       # "simulated namespace subsystem loaded (ABI v1)"
```

## ABI versioning

The `SHADOWNS_IOC_ABI_VERSION` ioctl returns `SHADOWNS_ABI_VERSION`. Clients must
check it on open (the reference client does) and refuse to run on a mismatch.
Bump the version in `include/uapi/shadowns.h` whenever the ioctl layout changes.
