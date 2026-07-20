# shadow_sysvipc — simulated System V IPC subsystem

`shadow_sysvipc/` is the SysV IPC subsystem source linked into the merged
`shadow_ctr.ko` (see `../README.md` for the umbrella overview). It keeps enough
SysV IPC bookkeeping alive on a kernel built **without `CONFIG_SYSVIPC`** for
container runtimes to stop tripping over `-ENOSYS` on the resource-management
syscalls.

It now works only through **transparent syscall hooks**: ftrace hooks hijack
the real `msgget`/`msgctl`, `semget`/`semctl`, and `shmget`/`shmctl` syscall
wrappers when the kernel's native implementation is missing, so **unmodified
stock `containerd`/`runc`/`dockerd`** can keep calling the normal SysV IPC
syscalls.

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
└───────────────────────────────────────────────────────────────────────────┘
                                                 │
                                                 ▼
                                 global svipc_resource registry
                        (xarray id map + keyed hash + refcounted objects)
                                                 │
                                                 ▼
                                          per-TGID refs
                                     (hooked syscall path)
```

### Internal model

* **Resource objects** are refcounted `svipc_resource` records keyed by
  `(type, key)` for non-private objects and by generated id for `IPC_PRIVATE`.
* Ownership is tracked per **task group** (`task_tgid_nr`) so
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
| `msgsnd(2)` / `msgrcv(2)` | Real in-kernel FIFO message queue: `copy_from_user`/`copy_to_user` payload transfer, `mtype` matching (including negative `msgtyp` "lowest type <= \|msgtyp\|" semantics), blocking send/receive with wakeups, `IPC_NOWAIT` and `MSG_NOERROR`. |
| `semget(2)` | Create/get virtual semaphore sets by key, remember `nsems`, return a fake id. |
| `semctl(2)` | `IPC_STAT`, `IPC_RMID`, and the value commands `GETVAL`/`SETVAL`/`GETALL`/`SETALL` on those virtual semaphore sets. |
| `semop(2)` / `semtimedop(2)` | Real atomic multi-op transaction semantics (see below): all-or-nothing apply against a real per-semaphore value array, blocking/wakeup, `IPC_NOWAIT`, and `semtimedop`'s timeout. |
| `shmget(2)` | Create/get virtual shared-memory segments by key, remember `size`, return a fake id. |
| `shmctl(2)` | `IPC_STAT` and `IPC_RMID` on those virtual shared-memory segments. |
| `shmat(2)` / `shmdt(2)` | Real shared mapping: a lazily-created `shmem` (tmpfs) file backs each segment, shared by every attach of the same id via `vm_mmap()`/`vm_munmap()`, so writes from one attached process are genuinely visible to every other attached process. |

Key-based lookups (`msgget`/`semget`/`shmget` with a non-`IPC_PRIVATE` key) are
scoped per simulated IPC namespace (see "Per-namespace scoping" below), so two
different simulated containers requesting the same key do not collide with
each other's resource.

### `semop`/`semtimedop` semantics

Vendored from real SysV semantics (see `shadow_sysvipc_sem.c`'s header
comment and `ipc/sem.c`): every op in a single `semop(2)` call must be
satisfiable as one atomic transaction against the *current* semaphore values
-- `sem_op > 0` always succeeds (increment), `sem_op == 0` blocks until the
value is exactly zero, `sem_op < 0` blocks until the value is `>= |sem_op|`
then decrements it. If any op in the array cannot proceed immediately, none
of them are applied: the caller gets `-EAGAIN` (`IPC_NOWAIT`) or blocks until
a later `semop(2)`/`semctl(2)` on the same set changes a value, at which
point the whole array is atomically re-evaluated from scratch.

**Not implemented:** `SEM_UNDO` (adjustment-on-exit bookkeeping) -- a process
that dies mid-critical-section while holding `SEM_UNDO`'d semaphores leaves
them at whatever value its own `semop(2)` calls last left them, instead of
having the kernel roll the adjustment back automatically. This is the same
class of deliberate, documented trade-off as `shadow_ns_pid.c`'s simplified
orphan-reparenting.

### `shmat`/`shmdt` semantics

Each segment's backing store is a single `shmem_kernel_file_setup()`-created
file, created on the *first* `shmat(2)` of a given `shmid` and shared by
every later attach of that same id -- exactly like real SysV shared memory,
every attaching process maps the same underlying pages via the ordinary page
cache. Each successful attach takes its own reference on the
`svipc_resource` (so the segment survives as long as anything is still
attached, even after the creating task group exits or `IPC_RMID`s it,
mirroring real "marked for destruction, but deferred until last detach"
semantics) and is tracked by `(tgid, address)` so `shmdt(2)` -- which only
receives an address -- can find and unmap exactly that mapping. Attachment
records for task groups that exit without calling `shmdt(2)` are reaped
opportunistically (no `vm_munmap()` needed in that case: the kernel's own
`exit_mmap()` already tore down that address space).

**Not implemented:** `SHM_LOCK`/`mlock` accounting, hugetlb-backed segments,
and `SHM_REMAP`/`SHM_EXEC` address-hint placement quirks -- out of scope for
the common container use case of a plain read/write shared mapping.

### Per-namespace scoping

`shadow_ns`'s `CLONE_NEWIPC` simulation is bookkeeping-only (see
`../shadow_ns/README.md`): it does not create a functionally isolated IPC
namespace, only a refcounted namespace-identity object. `shadow_sysvipc`
consults that object's id (`shadow_ns_current_ipc_ns_id()`, both subsystems
link into the same `shadow_ctr.ko`) to scope **key-based** lookups
(`svipc_find_key_locked()`) per simulated IPC namespace, mirroring the
*shape* of real `ipc/namespace.c`'s per-namespace `copy_ipcs()`/`free_ipcs()`
registries (not their storage, which is scaled down to shadow_ns's flat,
single-level nesting). Resource **ids** returned to userspace remain a
single global id space (like real SysV ids, which are also kernel-wide
unique, not per-namespace), so this only affects which existing resource a
non-`IPC_PRIVATE` key search matches.

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

`shadow_sysvipc` is still a **bookkeeping-plus-real-data-path layer**, not a
byte-for-byte drop-in replacement for native SysV IPC.

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
| `include/uapi/shadow_sysvipc.h` | Shared shadow SysV IPC constants + metadata structs. |
| `shadow_sysvipc_registry.c` / `shadow_sysvipc_tgid.c` / `shadow_sysvipc_hooks.c` / `shadow_sysvipc_internal.h` | Split implementation: resource registry, TGID ownership tracking, syscall hooks, and shared private declarations. |
| `../Makefile` | Unified out-of-tree build for `shadow_ctr.ko`. |

## Building / loading

```sh
make -C /path/to/kernel/build M="$PWD/shadow_ctr/shadow_ctr" modules
insmod shadow_ctr/shadow_ctr/shadow_ctr.ko
# stock userspace can now call msgget/msgctl/semget/semctl/shmget/shmctl normally
```

The module logs which syscall wrapper symbols were hooked through the shared
`shadow_hook` helper.
