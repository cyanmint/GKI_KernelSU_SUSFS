# shadow_mqueue — simulated POSIX message queue subsystem

`shadow_mqueue` is one of the subsystems linked into the combined
`shadow_ctr.ko` module (see `README.md` in this directory for the umbrella
overview). It supplies a working POSIX mqueue implementation on kernels built
without `CONFIG_POSIX_MQUEUE`.

It now works in **two** ways:

1. **Transparent mode** for unmodified `runc` / `containerd` / `dockerd`: the
   module ftrace-hooks the real `mq_*` syscalls and services them in-kernel when
   the built-in subsystem is missing.
2. **Legacy ioctl mode** via `/dev/shadow_mqueue`: kept unchanged for debugging
   and backwards compatibility.

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
  stock runtime             hooked mq_* syscalls           shadow_ctr.ko
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
* **Legacy ioctl sessions** still exist. They use the same internal helpers as
  the hooked syscalls; there is no second message-transfer implementation.
* A minimal `"mqueue"` pseudo filesystem type is also registered
  (`register_filesystem()`), independent of the syscall hooks above. Container
  runtimes such as `runc` unconditionally `mount("mqueue", "/dev/mqueue",
  "mqueue", ...)` during container init; without a registered `"mqueue"` fs
  type that mount fails with `-ENODEV` ("no such device") and container
  startup aborts before the hooked `mq_*` syscalls ever run. The mounted
  filesystem's contents are empty and irrelevant — all real queue state lives
  in the hash table above, not on this mount — it exists purely so that
  mount(2) call succeeds.

## What is simulated

| Operation             | Behaviour |
|-----------------------|-----------|
| `mq_open` / `MQOpen`  | Create or open a named queue. Transparent mode returns a real fd; ioctl mode returns the old per-session handle. |
| `mq_timedsend` / Send | Buffer a message; block, time out, or return immediately when full. |
| `mq_timedreceive` / Receive | Dequeue the highest-priority message; block, time out, or return immediately when empty. |
| `mq_unlink` / `MQUnlink` | Remove the name from the global table and wake blocked waiters. |
| `mq_getsetattr` / `GetAttr` / `SetAttr` | Read queue state and toggle `NONBLOCK`. |
| close / `MQClose`     | Drop the open-handle reference; anon-inode fds auto-clean up on close/exit. |

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
* The queue implementation is functional, but it is still a simulation: the
  registered `"mqueue"` filesystem type only exists so that
  `mount("mqueue", "/dev/mqueue", "mqueue", ...)` (as issued unconditionally
  by container runtimes such as `runc`) succeeds — it has no persistence
  beyond module-managed state and does not attempt to emulate every
  edge-case of in-tree `ipc/mqueue.c`.

## Files

| Path                           | Purpose |
|--------------------------------|---------|
| `include/uapi/shadow_mqueue.h` | Stable ioctl ABI and constants. |
| `shadow_mqueue.c`              | Queue engine, ioctl API, anon-fd bridge, and syscall hooks. |
| `Makefile`                     | Out-of-tree module build. |

## Building

```sh
cd ctr_patches/shadow_ctr
make KDIR=/path/to/kernel/build
```

Android GKI cross-build example:

```sh
make -C /path/to/kernel/build M=$(pwd) ARCH=arm64 LLVM=1 modules
```

## Loading

```sh
insmod shadow_ctr.ko
dmesg | grep shadow_mqueue
```

Expected log theme: the misc device is registered, and the `mq_*` hook set is
installed if those syscall wrappers are present.

## ABI versioning

`SHADOW_MQUEUE_IOC_ABI_VERSION` still reports `SHADOW_MQUEUE_ABI_VERSION` for
the legacy ioctl path. Bump it only if the ioctl layout changes incompatibly.
