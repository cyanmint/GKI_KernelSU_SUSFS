# shadow_ns — simulated ("shadow") namespace subsystem

`shadow_ns` is one of the subsystems linked into the combined `shadow_ctr.ko`
module (see `README.md` in this directory for the umbrella overview). It
provides an independent, reference-counted set of *shadow* namespace objects on
kernels that were built without the native `CONFIG_*_NS` features.

It now has **two front doors**:

* a transparent **syscall-mitm path** built on `shadow_hook.h`, so a stock,
  unmodified `containerd`/`runc`/`dockerd` can hit the shadow implementation by
  calling the real `unshare(2)`, `clone(2)`/`clone3(2)`/`fork(2)`,
  `sethostname(2)`, `setdomainname(2)` and `uname(2)` entry points; and
* the original `/dev/shadow_ns` **ioctl API**, kept unchanged for diagnostics,
  manual control and backward compatibility.

## Why this exists

The native Linux namespace machinery (`CONFIG_UTS_NS`, `CONFIG_IPC_NS`,
`CONFIG_PID_NS`, `CONFIG_NET_NS`, `CONFIG_USER_NS`, …) is compiled directly
into `vmlinux`. It adds fields to `task_struct`, `nsproxy` and `cred`, and
wires namespace syscalls into the core kernel. Those struct layouts are fixed
when the kernel image is built, so a module loaded afterwards cannot add *real*
namespaces to a running kernel.

`shadow_ns` therefore implements a **parallel shadow model**. It can make the
relevant syscalls succeed and preserve namespace identities/lifecycle, but it
can only provide the degree of behaviour described below; it cannot conjure the
missing in-kernel isolation machinery.

## Architecture

```text
                         stock containerd / runc / dockerd
                     ┌────────────────────────────────────────┐
                     │ unshare / clone3 / fork / sethostname │
                     └────────────────────┬───────────────────┘
                                          │ real syscalls
                                          ▼
                            shadow_ctr.ko + shadow_hook.h
          ┌───────────────────────────────────────────────────────────────┐
          │ ftrace hooks on syscall wrappers                             │
          │  • per-tgid current shadow namespaces                        │
          │  • child inheritance on clone/clone3/fork/vfork             │
          │  • UTS read/write interception                               │
          └────────────────────┬──────────────────────────────────────────┘
                               │
          ┌────────────────────▼──────────────────────────────────────────┐
          │ global id -> shadow_ns registry (xarray, refcounted objects) │
          └────────────────────┬──────────────────────────────────────────┘
                               │
                     ┌─────────▼─────────┐
                     │ /dev/shadow_ns     │
                     │ ioctl session API │
                     └───────────────────┘
```

### State model

* **Transparent mode is keyed by TGID.** The module keeps a small per-task-group
  table (`task_tgid_nr(current)` → `cur[TYPE]`) so ordinary syscalls can find
  the caller's current shadow namespaces without any userspace side channel.
  Dead TGID entries are reaped periodically by delayed work.
* **Ioctl mode is keyed by open fd.** Each `open("/dev/shadow_ns")` still creates
  one independent session exactly as before.
* **Shadow namespaces are global refcounted objects.** Both paths share the same
  underlying `shadow_ns` objects and stable integer ids.

## What is actually simulated

| Type    | Behaviour provided by shadow_ns                                            |
|---------|---------------------------------------------------------------------------|
| UTS     | **Functional** per-namespace `nodename` / `domainname` storage.           |
| IPC     | Reference-counted membership + parent lineage (bookkeeping only).          |
| MNT     | Reference-counted membership + parent lineage (bookkeeping only).          |
| PID     | Reference-counted membership + parent lineage (bookkeeping only).          |
| NET     | Reference-counted membership + parent lineage (bookkeeping only).          |
| USER    | Reference-counted membership + parent lineage (bookkeeping only).          |
| CGROUP  | Reference-counted membership + parent lineage (bookkeeping only).          |

## Transparent mode details

### `unshare(2)`

`shadow_ns` intercepts `CLONE_NEWUTS`, `CLONE_NEWIPC`, `CLONE_NEWNS`,
`CLONE_NEWPID`, `CLONE_NEWNET`, `CLONE_NEWUSER` and `CLONE_NEWCGROUP`. For each
shadow bit it creates/replaces the caller's per-TGID shadow namespace of that
type. Any remaining non-shadow unshare bits are passed through to the real
kernel implementation.

### `clone(2)` / `clone3(2)` / `fork(2)` / `vfork(2)`

Children inherit the parent's current shadow namespace set. If a clone request
contains shadow `CLONE_NEW*` bits, those bits are stripped before calling the
real kernel and the child receives newly-created shadow namespaces of the
requested types.

### UTS-related syscalls

If the current TGID has a shadow UTS namespace joined:

* `sethostname(2)` updates the shadow `nodename`
* `setdomainname(2)` updates the shadow `domainname`
* `uname(2)` / `newuname` returns the real kernel result with the shadow UTS
  fields overlaid

If no shadow UTS namespace is active, those syscalls fall straight through to
normal kernel behaviour.

### `setns(2)`

The hook calls the real `setns(2)` first. If that succeeds, nothing special
happens. If the real syscall fails with `-EINVAL`/`-ENOTTY`, `shadow_ns` offers
**best-effort shadow-only fallbacks**:

* if `fd` is actually a `/dev/shadow_ns` session fd, the current TGID joins that
  session's current shadow namespace of the requested type; or
* as a diagnostic/private encoding, if there is no real fd (`EBADF` on lookup),
  the numeric `fd` value is treated as a shadow namespace id.

## Honest scope / limitations

`shadow_ns` remains a **simulation and bookkeeping layer**, not a replacement for
real kernel isolation:

* **UTS is the only fully functional namespace type here.** Hostname and domain
  name become transparently per-shadow-namespace.
* **IPC/MNT/PID/NET/USER/CGROUP remain bookkeeping-only.** They provide stable
  ids, ancestry and join semantics, but do not enforce real kernel isolation of
  those subsystems.
* **Transparent `setns(2)` is intentionally partial.** This module does **not**
  synthesize shadow-tagged nsfs fds for `/proc/<pid>/ns/*` opens. Full,
  stock-runtime-transparent shadow `setns` by proc namespace fd would also
  require intercepting those opens and handing back shadow-aware fds; that is
  not implemented here because it is substantially more invasive VFS work.
* **Child shadow-state installation is best-effort after a successful fork-like
  syscall.** The hook strips shadow `CLONE_NEW*` bits before calling the real
  kernel, then mirrors shadow membership onto the new TGID. If that post-fork
  bookkeeping fails (for example due to allocation failure), the process still
  exists but its shadow state may be incomplete; the module logs a warning.
* **Dead TGID state is cleaned up best-effort, not synchronously at exit.** This
  implementation intentionally uses a periodic reaper instead of a task-exit
  tracepoint because the relevant tracepoint symbol is not reliably module-usable
  across kernels. Stale entries therefore disappear asynchronously.

## Files

| Path                      | Purpose                                                     |
|---------------------------|-------------------------------------------------------------|
| `include/uapi/shadow_ns.h` | Stable ioctl ABI shared with userspace.                     |
| `shadow_ns.c`              | The kernel module: ioctl path + transparent syscall hooks.  |
| `Makefile`                | Out-of-tree build (`make KDIR=...`).                        |

## Building

### Out-of-tree (produces `shadow_ctr.ko`)

```sh
cd ctr_patches/shadow_ctr
make KDIR=/path/to/kernel/build
sudo insmod shadow_ctr.ko
ls -l /dev/shadow_ns
```

For an Android GKI cross build, point `KDIR` at the prepared kernel `out`
directory and pass the same `ARCH=arm64 LLVM=1` flags used to build the kernel:

```sh
make -C /path/to/kernel/build M=$(pwd) ARCH=arm64 LLVM=1 modules
```

## Loading

```sh
insmod shadow_ctr.ko
# dmesg: loaded (ABI v1, device /dev/shadow_ns, transparent hooks N)
```

## ABI versioning

The `SHADOW_NS_IOC_ABI_VERSION` ioctl returns `SHADOW_NS_ABI_VERSION`. Manual
clients that use `/dev/shadow_ns` should check it on open and refuse to run on a
mismatch. Bump the version in `include/uapi/shadow_ns.h` whenever the ioctl
layout changes incompatibly.
