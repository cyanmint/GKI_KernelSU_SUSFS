# shadow_sysvipc — simulated System V IPC subsystem

`shadow_sysvipc` is a **standalone loadable kernel module** that provides
bookkeeping for virtual SysV IPC resources (message queues, semaphore sets, and
shared-memory segments) through ioctls on `/dev/shadow_sysvipc`. A **patched
containerd/runc** can talk to this ABI to manage virtual IPC objects on a GKI
kernel that was built **without** `CONFIG_SYSVIPC`.

## Why this exists

The native SysV IPC machinery (`CONFIG_SYSVIPC`) is compiled **directly into
`vmlinux`**.  It installs the `msgget`/`msgsnd`/`msgrcv`/`semget`/`semop`/
`shmget`/`shmat` family of syscalls and the `/proc/sysvipc` accounting.  Those
syscall table entries and struct layouts are frozen when the kernel image is
built, so a module loaded afterwards **cannot** add real SysV IPC support to a
running kernel.

`shadow_sysvipc` does not attempt the impossible.  Instead it maintains a
**parallel** set of virtual IPC objects that a patched runtime opts into
explicitly, giving it stable resource identities and key-based lookup semantics
without requiring in-kernel SysV support.

## Architecture

```
  patched containerd / runc               shadow_sysvipc.ko
  ┌──────────────────────┐  ioctl  ┌─────────────────────────────┐
  │ shadow_sysvipc.go    │────────▶│ /dev/shadow_sysvipc misc dev│
  │  Open()              │         │  per-fd "session"            │
  │  Create(TypeMsgQ, …) │         │   owned[] -> svipc_resource  │
  │  Destroy(id)         │         │  global id -> svipc_resource │
  └──────────────────────┘         │   (refcounted, xarray+hash)  │
                                   └─────────────────────────────┘
```

* **Session = open fd.** Each `open("/dev/shadow_sysvipc")` is one session.
  All references are dropped on `close()`.
* **Key-based lookup** mirrors `msgget(2)` / `semget(2)` / `shmget(2)`:
  `IPC_PRIVATE` creates a unique private object; a non-private key is stored in
  a hash table so a second session can get the same object.
* **Find-or-create is atomic**: the hash lookup and allocation are performed
  while holding the global map lock, so two concurrent creates with the same
  key produce exactly one new object.

## What is simulated

| Resource       | Behaviour provided by shadow_sysvipc                          |
|----------------|---------------------------------------------------------------|
| Message queue  | Create/get by key, stat, destroy.  No message passing.        |
| Semaphore set  | Create/get by key, stat (includes nsems), destroy.            |
| Shared memory  | Create/get by key, stat (includes size), destroy.             |

### Honest scope / limitations

`shadow_sysvipc` is a **bookkeeping layer**, not a replacement for real SysV IPC:

* Actual IPC operations (`msgsnd`/`msgrcv`, `semop`, `shmat`/`shmdt`) require
  kernel support and are **not** provided by this module.
* Use this module so that a patched runtime can proceed with resource-management
  calls without failing, and to maintain stable namespace-like identities across
  containers.
* If the container workloads themselves use SysV IPC, they will still fail unless
  `CONFIG_SYSVIPC` is compiled in.

## Files

| Path                                    | Purpose                                    |
|-----------------------------------------|--------------------------------------------|
| `include/uapi/shadow_sysvipc.h`         | Stable ioctl ABI shared with userspace.    |
| `shadow_sysvipc.c`                      | The kernel module.                         |
| `Makefile`                              | Out-of-tree build (`make KDIR=...`).       |
| `containerd/shadow_sysvipc.go`          | Reference Go client for containerd/runc.   |
| `containerd/README.md`                  | Runtime integration notes.                 |

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
insmod shadow_sysvipc.ko     # creates /dev/shadow_sysvipc (mode 0600)
dmesg | grep shadow_sysvipc  # "simulated SysV IPC subsystem loaded (ABI v1)"
```

## ABI versioning

`SHADOW_SYSVIPC_IOC_ABI_VERSION` returns `SHADOW_SYSVIPC_ABI_VERSION`.  Clients
must check it on open and refuse to run on a mismatch.  Bump in
`include/uapi/shadow_sysvipc.h` whenever the ioctl layout changes.
