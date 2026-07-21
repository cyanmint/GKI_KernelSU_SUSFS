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
* `lkm4ctr_diagfs.c` — the `lkm4ctr` diagnostics pseudo-filesystem: per-
  submodule load/unload control plus status/hooks/namespaces/log
  introspection, and the global `safe_unload` self-unload control (see
  below)

Their sources stay split by subsystem for maintainability, but they now build
and load only as one module with the single entry point in
`lkm4ctr_main.c`.

## No submodule is auto-loaded

`insmod lkm4ctr.ko` only brings up the shared hook engine
(`shadow_hijack`) and registers the `lkm4ctr` diagfs filesystem type --
none of `shadow_ns`/`shadow_sysvipc`/`shadow_mqueue`/`shadow_cgdevices` are
started automatically. Mount the diagfs and start what you need:

```sh
mount -t lkm4ctr diag /mnt
echo load > /mnt/modules/shadow_ns/status   # start just shadow_ns, or:
echo load > /mnt/safe_unload                # start every submodule at once
cat /mnt/modules/shadow_ns/status           # "active" / "not loaded"
```

Each submodule's `status` file also accepts `unload` (graceful) and
`force`/`force_unload`: every submodule's own `_exit()` already
unconditionally frees all of its resources and removes its hooks, so both
commands behave identically for that submodule today -- `force` additionally
brackets the call with a brief module-wide hook quiesce as an extra safety
net. `rmmod lkm4ctr` (and `safe_unload`, below) always force-clean up
whatever submodules are still active on the way out, regardless of whether
they were ever started via diagfs, so a submodule's own state can never
block module removal.

## rmmod safety

Every hooked call executed while a redirected call is in flight holds a
module reference for its duration (see `common/shadow_hook.h` and
`shadow_hijack/README.md`), so an ordinary `rmmod lkm4ctr` refuses to race
with in-flight calls: the kernel returns `-EBUSY` ("Module lkm4ctr is in
use") until they finish, instead of panicking once their code is freed out
from under them.

## Safe unload via the diagfs

Writing `1` (or `unload`/`remove`/`graceful`) to `./mnt/safe_unload`
triggers the module to unload itself with no further operator action:

```sh
echo 1 > /mnt/safe_unload
```

This spawns a worker thread that quiesces every hook (stopping new
redirected calls from starting), waits for any already in-flight calls to
finish, then explicitly frees every submodule's resources, and finally
launches a real userspace `rmmod lkm4ctr` on its own. If in-flight calls
don't drain within 30 seconds, the attempt is aborted, hooks resume normal
operation, and the module stays loaded. Writing `force`/`force_unload`
instead runs the same sequence, except every submodule's resources are
freed immediately after quiescing rather than waiting until the very end.
Reading `./mnt/safe_unload` reports `idle` or `in-progress` plus its own
log tail.

This control lives on the `lkm4ctr` diagfs (`mount -t lkm4ctr diag <mnt>`)
rather than a sysfs attribute or misc device: see `lkm4ctr_diagfs.c`'s file
header for the full rationale and the complete list of files it exposes.

## Build

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr/lkm4ctr" modules
```

For Android GKI, build against the matching DDK `vmlinux`/`Module.symvers`
sysroot exactly as described in `../README.md`.
