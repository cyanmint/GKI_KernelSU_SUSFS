# lkm4ctr.ko

`lkm4ctr/lkm4ctr/` builds the single merged `lkm4ctr.ko` loadable
kernel module.

## Layout

The unified module links these internal subsystem source trees together:

* `shadow_hijack/` — shared ftrace/kprobe hook implementation
* `shadow_ns/` — namespace hooks and fallback simulation
* `shadow_sysvipc/` — SysV IPC hooks and registry
* `shadow_mqueue/` — POSIX mqueue hooks and queue engine
* `shadow_cgdevices/` — device-open compatibility hooks
* `lkm4ctr_safe_unload.c` — sysfs-triggered self-unload (see below)

Their sources stay split by subsystem for maintainability, but they now build
and load only as one module with the single entry point in
`lkm4ctr_main.c`.

## rmmod safety

Every hooked call executed while a redirected call is in flight holds a
module reference for its duration (see `common/shadow_hook.h` and
`shadow_hijack/README.md`), so an ordinary `rmmod lkm4ctr` refuses to race
with in-flight calls: the kernel returns `-EBUSY` ("Module lkm4ctr is in
use") until they finish, instead of panicking once their code is freed out
from under them.

## Safe unload via sysfs

Writing `1` (or `unload`/`remove`) to `/sys/module/lkm4ctr/safe_unload`
triggers the module to unload itself with no further operator action:

```sh
echo 1 > /sys/module/lkm4ctr/safe_unload
```

This spawns a worker thread that quiesces every hook (stopping new
redirected calls from starting), waits for any already in-flight calls to
finish, and then launches a real userspace `rmmod lkm4ctr` on its own. If
in-flight calls don't drain within 30 seconds, the attempt is aborted, hooks
resume normal operation, and the module stays loaded. Reading the file
reports `idle` or `in-progress`.

## Build

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr/lkm4ctr" modules
```

For Android GKI, build against the matching DDK `vmlinux`/`Module.symvers`
sysroot exactly as described in `../README.md`.
