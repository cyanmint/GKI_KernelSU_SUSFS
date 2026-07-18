# shadow_mqueue — simulated POSIX message queue subsystem

`shadow_mqueue` is a **standalone, independently loadable** module
(`shadow_mqueue.ko`) — one of the modules in the `shadow_ctr/` family (see
`../README.md` for the umbrella overview). It supplies a working POSIX mqueue
implementation on kernels built without `CONFIG_POSIX_MQUEUE`. It has no
dependency on any other shadow module.

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
  stock runtime             hooked mq_* syscalls         shadow_mqueue.ko
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
* `shadow_mqueue` deliberately does **not** register a `"mqueue"` filesystem
  type. An earlier revision did so via `get_tree_nodev()`/`simple_fill_super()`
  so that a container runtime's unconditional
  `mount("mqueue", "/dev/mqueue", "mqueue", ...)` call would succeed even
  without `CONFIG_POSIX_MQUEUE`. However, those two VFS helpers are trimmed
  from the exported-symbol table of production GKI kernels (unreferenced by
  any built-in code, so `CONFIG_TRIM_UNUSED_KSYMS` drops their
  `EXPORT_SYMBOL` entries), which made the *entire module* fail to load with
  `insmod: failed to load shadow_mqueue.ko: No such file or directory`
  (the kernel's module loader surfaces an unresolved symbol as `-ENOENT`).
  The mq_\* syscall hooks above are fully self-contained regardless of
  whether `"mqueue"` is mounted, so dropping the pseudo-filesystem
  registration does not affect message-queue semantics — it only means a
  runtime's `mount("mqueue", ...)` call will fail on kernels without
  `CONFIG_POSIX_MQUEUE` (or another provider of that filesystem type).

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
  register a `"mqueue"` filesystem type (see "Architecture" above), so
  `mount("mqueue", "/dev/mqueue", "mqueue", ...)` still requires
  `CONFIG_POSIX_MQUEUE` or another provider of that filesystem type.

## Files

| Path                           | Purpose |
|--------------------------------|---------|
| `include/uapi/shadow_mqueue.h` | Shared mqueue constants and attribute layout. |
| `shadow_mqueue.c`              | Queue engine, anon-fd bridge, and syscall hooks. |
| `Makefile`                     | Out-of-tree module build. |

## Building

```sh
cd shadow_ctr/shadow_mqueue
make KDIR=/path/to/kernel/build
```

Android GKI cross-build example:

```sh
make -C /path/to/kernel/build M=$(pwd) ARCH=arm64 LLVM=1 modules
```

## Loading

```sh
insmod shadow_mqueue.ko
dmesg | grep shadow_mqueue
```

Expected log theme: the `mq_*` hook set is installed if those syscall
wrappers are present.
