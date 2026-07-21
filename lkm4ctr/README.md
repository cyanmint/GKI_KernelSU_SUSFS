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
* `lkm4ctr_diagfs.c` — the `lkm4ctr` diagnostics pseudo-filesystem: runtime
  control/status plus hooks/namespaces/log/resource introspection

Their sources stay split by subsystem for maintainability, but they now build
and load only as one module with the single entry point in
`lkm4ctr_main.c`.

## No submodule is auto-loaded

`insmod lkm4ctr.ko` only brings up the shared hook engine (`shadow_hijack`)
and registers the `lkm4ctr` diagfs filesystem type — none of
`shadow_ns`/`shadow_sysvipc`/`shadow_mqueue`/`shadow_cgdevices` are started
automatically. Mount the diagfs and start what you need:

```sh
mount -t lkm4ctr diag /mnt
echo load > /mnt/ns/control          # start just shadow_ns, or:
echo load > /mnt/global/control      # start every submodule at once
cat /mnt/ns/status                   # unloaded / loading / active / ...
cat /mnt/ns/pid/namespaces           # pid-only namespace membership listing
```

Each runtime-loadable submodule exposes `control`, `status`, `log`, and (where
relevant) `hooks` or a live-state listing file directly under the mount root:

* `/mnt/global/{control,status,log,resources}`
* `/mnt/hijack/{control,status,log,functions}`
* `/mnt/ns/{control,status,hooks,log,namespaces}` plus
  `/mnt/ns/{pid,ipc,mnt,net,user,uts,cgroup}/...`
* `/mnt/sysvipc/{control,status,hooks,resources,log}`
* `/mnt/mqueue/{control,status,hooks,log,msg}`
* `/mnt/cgroupdevices/{control,status,hooks,log}`

Per-submodule `control` accepts `load`, `unload` (`remove`/`graceful` aliases),
and `forceunload` (`force`/`force_unload` aliases). `status` is read-only and
prints exactly one lifecycle state: `unloaded`, `loading`, `active`,
`graceful unloading`, or `force unloading`. `shadow_hijack` keeps the same
layout for symmetry, but its control file only reports help because the shared
hook engine itself is always active while `lkm4ctr.ko` is loaded.

`rmmod lkm4ctr` (and `global/control`, below) always force-clean up whatever
submodules are still active on the way out, so a submodule's own state can
never block module removal.

## rmmod safety

Every hooked call executed while a redirected call is in flight holds a module
reference for its duration (see `common/shadow_hook.h` and
`shadow_hijack/README.md`), so an ordinary `rmmod lkm4ctr` refuses to race
with in-flight calls: the kernel returns `-EBUSY` ("Module lkm4ctr is in use")
until they finish, instead of panicking once their code is freed out from under
them.

## Global unload via the diagfs

Writing `unload` (or `1`/`remove`/`graceful`) to `./mnt/global/control`
triggers the module to unload itself with no further operator action:

```sh
echo unload > /mnt/global/control
```

This spawns a worker thread that quiesces every hook (stopping new redirected
calls from starting), waits for any already in-flight calls to finish, then
explicitly frees every submodule's resources, and finally launches a real
userspace `rmmod lkm4ctr` on its own. If in-flight calls don't drain within 30
seconds, the attempt is aborted, hooks resume normal operation, and the module
stays loaded. Writing `forceunload`/`force`/`force_unload` instead runs the
same sequence, except every submodule's resources are freed immediately after
quiescing rather than waiting until the very end.

Reading `./mnt/global/control` prints the accepted commands; reading
`./mnt/global/status` prints the current lifecycle state. `./mnt/global/log`
contains the combined log stream and `./mnt/global/resources` aggregates the
live resources still tracked across the linked subsystems.

## Build

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr/lkm4ctr" modules
```

For Android GKI, build against the matching DDK `vmlinux`/`Module.symvers`
sysroot exactly as described in `../README.md`.
