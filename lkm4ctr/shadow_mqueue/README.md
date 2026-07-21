# shadow_mqueue — simulated POSIX message queue subsystem

`shadow_mqueue/` is the POSIX mqueue subsystem source linked into the merged
`lkm4ctr.ko` (see `../README.md` for the umbrella overview). It supplies a
working POSIX mqueue implementation on kernels built without
`CONFIG_POSIX_MQUEUE`.

It now works only in **transparent mode** for unmodified `runc` /
`containerd` / `dockerd`: the module ftrace-hooks the real `mq_*` syscalls and
services them in-kernel when the built-in subsystem is missing.

## Why this exists

The native POSIX mqueue subsystem is compiled into `vmlinux`; it cannot be added
later as a normal LKM. But `runc` uses POSIX message queues for parent↔child
init synchronisation, so a GKI kernel with `CONFIG_POSIX_MQUEUE=n` breaks stock
container startup with `-ENOSYS`.

`shadow_mqueue` fills that gap. Message transfer is real: senders enqueue bytes
into a priority-ordered kernel list, receivers dequeue them, and both sides can
block with timeout semantics close to native `mq_timedsend(2)` /
`mq_timedreceive(2)`.

## Architecture

```
  stock runtime             hooked mq_* syscalls         lkm4ctr.ko
  ┌───────────────┐         ┌────────────────────┐         ┌──────────────────────┐
  │ mq_open()     │────────▶│ __arm64_sys_mq_*   │────────▶│ shadow internal engine│
  │ mq_timedsend()│         │ (or sys_mq_*)      │         │ name hash + waitqs    │
  │ mq_timedrecv()│         └────────────────────┘         │ real buffering        │
  └───────────────┘                                         └──────────────────────┘
          │
          └─ mq_open() returns a real anon-inode-backed fd ("mqd_t")
             so normal close-on-exec / close(2) / task-exit cleanup still works.
```

Key pieces:

* **Named queues** live in a global hash table keyed by queue name.
* **Messages** are stored in per-queue priority lists (`highest prio first`).
* **Blocking** is implemented with wait queues for senders and receivers.
* **Handle lifetime** is backed by a real anonymous file descriptor from
  `anon_inode_getfd()`. That is the trick that makes hooked `mq_open()` return a
  genuine fd-like object, so later `mq_timedsend()` / `mq_timedreceive()` /
  `mq_getsetattr()` calls from the same unmodified process can resolve the queue
  through `fdget()` and `file->private_data`.
* `shadow_mqueue` does **not** register a `"mqueue"` filesystem type of its
  own. An earlier revision did so via `get_tree_nodev()`/`simple_fill_super()`
  so that a container runtime's unconditional
  `mount("mqueue", "/dev/mqueue", "mqueue", ...)` call would succeed even
  without `CONFIG_POSIX_MQUEUE`. However, those two VFS helpers are trimmed
  from the exported-symbol table of production GKI kernels (unreferenced by
  any built-in code, so `CONFIG_TRIM_UNUSED_KSYMS` drops their
  `EXPORT_SYMBOL` entries), which made the *entire module* fail to load with
  `insmod: failed to load lkm4ctr.ko: No such file or directory`
  (the kernel's module loader surfaces an unresolved symbol as `-ENOENT`).
* Instead, `shadow_mqueue` hooks `mount(2)` itself (see "mount(\"mqueue\", ...)
  fallback" below): it lets the real `mount(2)` run first, and only when that
  fails with `-ENODEV` for fstype `"mqueue"` does it transparently retry the
  exact same call as a `tmpfs` mount. This covers both a kernel genuinely
  missing `CONFIG_POSIX_MQUEUE` *and* the case where `CONFIG_POSIX_MQUEUE` is
  real/builtin but the mount still fails with `-ENODEV` in the shadow_ns
  family's fake-namespace environment (pid/mnt/user namespaces reported as
  "shadow_ns bookkeeping" rather than real by `lkm4ctr_checker` can confuse
  the real mqueue filesystem's per-namespace tree lookup). No trimmed symbol
  is ever referenced: the fallback reuses the real `mount(2)` syscall itself
  (via `vm_mmap()`/`vm_munmap()`, ordinary VFS/ELF-loader helpers that are
  never trimmed) with its filesystem-type argument swapped for `"tmpfs"`.
* `shadow_mqueue` also proactively creates and mounts `/dev/mqueue` itself
  at module load time (see "Proactive `/dev/mqueue` creation" below), instead
  of only reacting to a `mount(2)` call that some userspace process may or
  may not make after the module is loaded.

### `mount("mqueue", ...)` fallback

Container runtimes (`dockerd`/`containerd`/`runc`) unconditionally attempt
`mount("mqueue", "/dev/mqueue", "mqueue", MS_NOSUID|MS_NODEV|MS_NOEXEC, ...)`
during container init; a failure surfaces to users as an OCI runtime error
such as:

```
error mounting "mqueue" to rootfs at "/dev/mqueue": ... no such device: unknown
```

`shadow_mqueue` hooks `mount(2)` and lets the real call run first. Only when
it fails with `-ENODEV` *and* the requested fstype is exactly `"mqueue"` does
it retry the identical call with the fstype swapped for `"tmpfs"` (same
`dev_name`/`dir_name`/`flags`/`data`, so options like `mode=`/`size=` that
runtimes pass are preserved). Every other fstype, and every other mount
error, passes straight through untouched. This makes `/dev/mqueue` a real,
working mountpoint for the runtime regardless of whether the kernel's mqueue
subsystem is builtin, shadowed, or simply fails to mount in this particular
namespace-faking setup.

### Proactive `/dev/mqueue` creation

The `mount(2)` hook above only helps if some process actually calls
`mount("mqueue", "/dev/mqueue", ...)` *after* `lkm4ctr.ko` is loaded.
In practice the module is typically insmod'd late (e.g. as a KernelSU/Magisk
post-fs-data module), well after init.rc's own one-shot
`mount mqueue mqueue /dev/mqueue ...` line already ran and silently failed
with `-ENODEV` (init never retries a failed boot-time mount). That would
otherwise leave `/dev/mqueue` nonexistent, or an empty, never-mounted
directory, for the rest of boot.

To fix this, `shadow_mqueue_init()` also creates `/dev/mqueue` (if it doesn't
already exist) and mounts `tmpfs` on it directly, the same way the `mount(2)`
hook's fallback would have, without waiting for a `mount(2)` call that may
never come. If `/dev/mqueue` is already a mountpoint (real `mqueue`, or a
mount left over from a previous load), it is left untouched. None of the VFS
helpers this needs (`path_mount()`, `vfs_mkdir()`, `kern_path_create()`,
`done_path_create()`) are referenced directly by name: `path_mount()` isn't
`EXPORT_SYMBOL()`'d on any target KMI, and `vfs_mkdir()` is
`EXPORT_SYMBOL_NS()`'d under `ANDROID_GKI_VFS_EXPORT_ONLY` on several GKI
branches, a namespace reserved for a small in-tree allow-list. All four are
instead resolved at runtime via the same `register_kprobe()`-based
`shadow_hook_resolve()` used for the mq_*/mount syscall hooks, which walks
kallsyms directly and doesn't care whether a symbol is exported, namespaced,
or trimmed. This step is best-effort: if any helper fails to resolve, or the
mount fails, `shadow_mqueue` logs it and continues relying on the reactive
`mount(2)` hook instead.


## What is simulated

| Operation             | Behaviour |
|-----------------------|-----------|
| `mq_open` | Create or open a named queue and return a real fd. |
| `mq_timedsend` | Buffer a message; block, time out, or return immediately when full. |
| `mq_timedreceive` | Dequeue the highest-priority message; block, time out, or return immediately when empty. |
| `mq_unlink` | Remove the name from the global table and wake blocked waiters. |
| `mq_getsetattr` | Read queue state and toggle `NONBLOCK`. |
| close | Drop the open-handle reference; anon-inode fds auto-clean up on close/exit. |

## Honest scope / limitations

* `mq_notify` is **not** implemented for shadow-backed queues yet. The hooked
  path currently returns `-ENOSYS` for those fds instead of faking signal or
  thread notifications.
* Transparent hooking only targets the **native 64-bit syscall ABI** right now.
  That matches the existing ioctl module's expectations; compat / 32-bit tasks
  are not wired up.
* `SHADOW_MQ_MSGSIZE_MAX` (256 bytes) and `SHADOW_MQ_MAXMSG_MAX` (1024 msgs)
  are sized for runtime init-pipe traffic, not for arbitrary large general-use
  mqueue workloads.
* Blocking waits still round nanosecond deadlines to jiffies.
* The queue implementation is functional, but it is still a simulation: it
  has no persistence beyond module-managed state and does not attempt to
  emulate every edge-case of in-tree `ipc/mqueue.c`. It also does not
  register a real `"mqueue"` filesystem type (see "Architecture" above); a
  `mount("mqueue", "/dev/mqueue", "mqueue", ...)` call is instead served by
  the `mount(2)` hook, which mounts `tmpfs` in place of `mqueue` only when the
  real mount fails with `-ENODEV`.

## Files

| Path                           | Purpose |
|--------------------------------|---------|
| `include/uapi/shadow_mqueue.h` | Shared mqueue constants and attribute layout. |
| `shadow_mqueue_core.c` / `shadow_mqueue_io.c` / `shadow_mqueue_hooks.c` / `shadow_mqueue_mount.c` / `shadow_mqueue_internal.h` | Split implementation: queue core, message I/O, syscall hooks, mount/init path, and shared private declarations. |
| `../Makefile`                  | Unified out-of-tree build for `lkm4ctr.ko`. |

## Building / loading

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr/lkm4ctr" modules
insmod lkm4ctr/lkm4ctr/lkm4ctr.ko
dmesg | grep shadow_mqueue
```

Expected log theme: the `mq_*` hook set is installed if those syscall
wrappers are present, and `/dev/mqueue` is created/mounted (or a reason is
logged for why it wasn't).
