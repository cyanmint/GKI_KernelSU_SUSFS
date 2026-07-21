# shadow_cgdevices — simulated cgroup device controller

`shadow_cgdevices/` is the cgroup-device compatibility subsystem source linked
into the merged `lkm4ctr.ko` (see `../README.md` for the umbrella overview).
It now only retains the transparent device-open hook points; the old
misc-device/ioctl-configured shadow rule store was removed.

## Why this exists

When `CONFIG_CGROUP_DEVICE=n`, there is no native device-controller rule store
and no native enforcement hook such as `devcgroup_check_permission()` to
intercept. Recent `runc` / `containerd` / `dockerd` usually tolerate that and
skip configuring a missing `devices` controller instead of hard-failing
container startup.

## Architecture

```text
lkm4ctr.ko
┌────────────────────────────────────────────────────────────┐
│ ftrace/kprobe hook shim                                   │
│   chrdev_open() / blkdev_open()*                          │
│            │                                               │
│            └── currently preserves native behaviour        │
└────────────────────────────────────────────────────────────┘
```

`*` block-device coverage is best-effort and symbol-name/version dependent;
character-device coverage through `chrdev_open()` is the stable path.

## What remains

The module still installs transparent hooks with the shared `shadow_hook`
helper at these kernel entry points:

| Kernel entry point | Status | Notes |
|--------------------|--------|-------|
| `chrdev_open`      | Hooked | Primary stable hook for character devices. |
| `blkdev_open`      | Best effort | Hook installed only if that symbol exists with the expected prototype. |

## Honest scope / limitations

With the ioctl control plane gone, there is currently **no remaining shadow
policy/rule provisioning path** in this module. The hooks therefore preserve the
native kernel result rather than enforcing a shadow allow/deny policy.

Other limitations:

* This module does **not** fake the cgroupfs `devices.*` files.
* Block-device coverage is version-dependent because the block open path has
  moved around more than the character-device path.
* On kernels without the native controller, this module is now effectively a
  hook-only compatibility shim / presence marker.

## Files

| Path                              | Purpose |
|-----------------------------------|---------|
| `include/uapi/shadow_cgdevices.h` | Shared cgroup-device rule constants. |
| `shadow_cgdevices.c`              | Kernel module: transparent device-open hook shim. |
| `../Makefile`                     | Unified out-of-tree build for `lkm4ctr.ko`. |

## Building / loading

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr/lkm4ctr" modules
insmod lkm4ctr/lkm4ctr/lkm4ctr.ko
dmesg | grep shadow_cgdevices
```

Expect a log line showing how many transparent hooks were installed on this
kernel.
