# shadow_ctr.ko

`shadow_ctr/shadow_ctr/` builds the single merged `shadow_ctr.ko` loadable
kernel module.

## Layout

The unified module links these internal subsystem source trees together:

* `shadow_hijack/` — shared ftrace/kprobe hook implementation
* `shadow_ns/` — namespace hooks and fallback simulation
* `shadow_sysvipc/` — SysV IPC hooks and registry
* `shadow_mqueue/` — POSIX mqueue hooks and queue engine
* `shadow_cgdevices/` — device-open compatibility hooks

Their sources stay split by subsystem for maintainability, but they now build
and load only as one module with the single entry point in
`shadow_ctr_main.c`.

## Build

```sh
make -C /path/to/kernel/build M="$PWD/shadow_ctr/shadow_ctr" modules
```

For Android GKI, build against the matching DDK `vmlinux`/`Module.symvers`
sysroot exactly as described in `../README.md`.
