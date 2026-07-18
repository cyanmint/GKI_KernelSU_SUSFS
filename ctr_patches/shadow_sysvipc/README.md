# shadow_sysvipc — simulated System V IPC subsystem

`shadow_sysvipc` is a **standalone loadable kernel module** that keeps enough
SysV IPC bookkeeping alive on a kernel built **without `CONFIG_SYSVIPC`** for
container runtimes to stop tripping over `-ENOSYS` on the resource-management
syscalls.

It now has **two front doors** backed by the same internal object registry:

1. **Transparent mode (new):** ftrace hooks hijack the real `msgget`/`msgctl`,
   `semget`/`semctl`, and `shmget`/`shmctl` syscall wrappers when the kernel's
   native implementation is missing, so **unmodified stock
   `containerd`/`runc`/`dockerd`** can keep calling the normal SysV IPC syscalls.
2. **Explicit mode (unchanged):** the original `/dev/shadow_sysvipc` misc-device
   ioctl ABI remains available for callers that want to drive the bookkeeping
   directly.

## Why this exists

The native SysV IPC subsystem (`CONFIG_SYSVIPC`) is compiled directly into
`vmlinux`. The syscall implementations, `/proc/sysvipc` integration, IPC object
layouts, shared-memory VM plumbing, and semaphore/message-queue data paths are
all decided when the kernel image is built. A module loaded later cannot bolt
those pieces back on in full.

So `shadow_sysvipc` does the limited thing a module *can* do safely: maintain a
parallel registry of virtual SysV IPC objects with stable ids, key lookups, and
lifecycle tracking.

## Architecture

```text
stock or patched userspace
┌───────────────────────────────────────────────────────────────────────────┐
│ msgget/msgctl/semget/semctl/shmget/shmctl syscalls                       │
│        │                                                                  │
│        ├── native CONFIG_SYSVIPC=y kernel ───────▶ real kernel SysV IPC  │
│        │                                                                  │
│        └── CONFIG_SYSVIPC=n kernel ──ftrace──▶ shadow_sysvipc hooks      │
│                                                │                           │
│ /dev/shadow_sysvipc ioctls ────────────────────┘                           │
└───────────────────────────────────────────────────────────────────────────┘
                                                 │
                                                 ▼
                                 global svipc_resource registry
                        (xarray id map + keyed hash + refcounted objects)
                                                 │
                        ┌────────────────────────┴────────────────────────┐
                        │                                                 │
                 per-open-fd session refs                         per-TGID refs
                 (ioctl path, unchanged)                         (hooked syscall path)
```

### Internal model

* **Resource objects** are refcounted `svipc_resource` records keyed by
  `(type, key)` for non-private objects and by generated id for `IPC_PRIVATE`.
* **Transparent syscall mode** reuses the same create/stat/destroy helpers as
  the ioctl path, but ownership is tracked per **task group** (`task_tgid_nr`) so
  a later `msgctl(IPC_RMID)` / `semctl(IPC_RMID)` / `shmctl(IPC_RMID)` from that
  process can drop the same synthetic reference.
* **Lazy TGID reaping:** because the hooked path does not extend `task_struct`
  and does not sleep from process-exit tracepoint context, dead task-group
  ownership is reaped opportunistically on the next intercepted SysV IPC
  syscall. That means leaked bookkeeping can survive briefly after process exit,
  but it is reclaimed the next time the transparent path is exercised.

## What is simulated

| Syscall family | Transparent behaviour provided |
|---|---|
| `msgget(2)` | Create/get virtual message queues by key, return a stable positive fake id. |
| `msgctl(2)` | `IPC_STAT` and `IPC_RMID` on those virtual queues. |
| `semget(2)` | Create/get virtual semaphore sets by key, remember `nsems`, return a fake id. |
| `semctl(2)` | `IPC_STAT` and `IPC_RMID` on those virtual semaphore sets. |
| `shmget(2)` | Create/get virtual shared-memory segments by key, remember `size`, return a fake id. |
| `shmctl(2)` | `IPC_STAT` and `IPC_RMID` on those virtual shared-memory segments. |
| `/dev/shadow_sysvipc` ioctls | Same create/stat/destroy ABI as before. |

### `IPC_STAT` payloads

The module zero-initialises the returned `struct msqid_ds` / `semid_ds` /
`shmid_ds` and fills only the fields it can honestly back with real shadow
state:

* queue/set/segment key (`msg_perm` / `sem_perm` / `shm_perm.key`)
* permission bits (`mode`)
* `msg_qbytes` (reported as the normal Linux default `MSGMNB`)
* `sem_nsems`
* `shm_segsz`

Everything else is reported as zero because there is no real kernel SysV IPC
subsystem keeping those counters and timestamps.

## Honest scope / limitations

`shadow_sysvipc` is still a **bookkeeping layer**, not a drop-in replacement for
native SysV IPC.

### Intentionally left unimplemented

* **`msgsnd(2)` / `msgrcv(2)`**: no real in-kernel message queue or payload
  storage exists here. Faking success would require copy-from/to-user plumbing,
  blocking wakeups, queue capacity accounting, and exact SysV semantics.
* **`semop(2)` / `semtimedop(2)`**: real SysV semaphore transactions require
  atomic multi-op semantics, waiting rules, wakeups, and `SEM_UNDO` handling.
* **`shmat(2)` / `shmdt(2)`**: there is no genuine shared-memory VM object to
  map. Returning a made-up success pointer would be unsafe.

For those syscalls the hook deliberately preserves the kernel's existing
behaviour: on a `CONFIG_SYSVIPC=n` build they still fail (typically `-ENOSYS`);
on a kernel with real SysV IPC enabled they continue to use the native kernel
implementation.

### Behavioural differences vs real SysV IPC

* Returned ids are **synthetic opaque positive integers**, not Linux's native
  slot+sequence encoded SysV ids. Container runtimes normally round-trip them
  opaquely, so this is sufficient for bookkeeping.
* `IPC_RMID` in transparent mode drops the calling task group's owned shadow
  reference, mirroring the original ioctl/session bookkeeping model rather than
  the kernel's full native lifetime rules.
* There is no `/proc/sysvipc` integration, namespace accounting, permission
  enforcement beyond stored mode bits, or real payload/state sharing between
  processes.

If container *workloads themselves* rely on actual SysV IPC payload transfer,
shared mappings, or semaphore synchronisation, they still need a kernel built
with real `CONFIG_SYSVIPC=y`.

## Files

| Path | Purpose |
|---|---|
| `include/uapi/shadow_sysvipc.h` | Stable ioctl ABI shared with userspace. |
| `shadow_sysvipc.c` | Module implementation: resource registry, ioctl API, syscall hooks. |
| `Makefile` | Out-of-tree build (`make KDIR=...`). |

## Building

```sh
cd ctr_patches/shadow_sysvipc
make KDIR=/path/to/kernel/build
sudo insmod shadow_sysvipc.ko
ls -l /dev/shadow_sysvipc
```

For an Android GKI cross build:

```sh
make -C /path/to/kernel/build M=$(pwd) ARCH=arm64 LLVM=1 modules
```

## Loading

```sh
insmod shadow_sysvipc.ko
# /dev/shadow_sysvipc still exists for the ioctl ABI
# stock userspace can now call msgget/msgctl/semget/semctl/shmget/shmctl normally
```

The module logs which syscall wrapper symbols were hooked through the shared
`shadow_hook` helper.

## ABI versioning

`SHADOW_SYSVIPC_IOC_ABI_VERSION` returns `SHADOW_SYSVIPC_ABI_VERSION`. Direct
ioctl clients should still check it on open and refuse to run on a mismatch.
Bump `include/uapi/shadow_sysvipc.h` whenever the ioctl layout changes.
