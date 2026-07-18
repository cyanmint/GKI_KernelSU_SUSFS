# shadow_overlay2 — standalone vendored overlay filesystem module

`shadow_overlay2.ko` registers a real, working `"overlay"` filesystem type on
GKI kernels that ship without it. It is built from `overlay/*.c`.

## Why this exists

`dockerd`'s `overlay2` storage driver (and `containerd`/`runc`) needs the
kernel to actually register an `"overlay"` filesystem type; it does not merely
check `/proc/config.gz`. Stock/production Android GKI boot images are generally
built with `CONFIG_OVERLAY_FS` disabled entirely (no `overlay.ko` either), so
`overlay2` fails to initialize with an error such as:

```
failed to mount overlay: invalid argument
error creating overlay mount to /var/lib/docker/overlay2/.../merged: invalid argument
```

Loading `shadow_overlay2.ko` provides the genuine behaviour behind that
requirement.

## What this is (and isn't)

Unlike the `shadow_*` *simulation* modules, this is **not** a
reimplementation/simulation — it is the real, unmodified `fs/overlayfs`
sources from the `android14-6.1` branch of
https://android.googlesource.com/kernel/common, packaged as a standalone
loadable module. Relative to the sources as originally vendored into the old
combined `shadow_ctr.ko`, the only module-wiring changes are:

- `overlay/super.c` now carries its own `module_init(shadow_overlay_init)` /
  `module_exit(shadow_overlay_exit)` and `MODULE_LICENSE`/`MODULE_AUTHOR`/
  `MODULE_DESCRIPTION`/`MODULE_VERSION` metadata (previously these lived once
  in the combined module's `shadow_ctr_main.c`);
- it no longer includes the old `shadow_ctr_internal.h`;
- it exports `shadow_overlay2_is_active()` (a presence marker consumed by
  `shadow_ctr_checker`).

The only functional change relative to upstream `fs/overlayfs` is the same
compatibility fix already carried by
[`../../ctr_patches/a14-6.1/`](../../ctr_patches/a14-6.1)'s
`overlayfs_dont_make_DCACHE_OP_HASH_and_DCACHE_OP_COMPARE_weird.patch`
(dropping the `DCACHE_OP_HASH`/`DCACHE_OP_COMPARE` bits from
`ovl_dentry_weird()` in `overlay/util.c`, required for overlayfs to work on top
of case-insensitive filesystems as used by modern Android).

## Kernel version scope — android14-6.1 ONLY

**This module only builds cleanly against android14-6.1 (`6.1.y`).**
Overlayfs's internal VFS ABI (`vfs_tmpfile_open`, `lookup_one`, `mnt_idmap` vs
`user_namespace` parameters, `struct renamedata` fields, etc.) changed across
kernel versions, so the same source does not compile unmodified against the
other KMIs (`android12-5.10`, `android13-5.10`, `android13-5.15`,
`android14-5.15`, `android15-6.6`, `android16-6.12`). Porting to those would
need a per-KMI source snapshot of `fs/overlayfs`.

Consequently the CI matrix builds `shadow_overlay2.ko` **only** for
android14-6.1, exactly as the old combined build gated it behind
`WITH_SHADOW_OVERLAY`. There is no build flag to gate anymore — simply do not
attempt to build this directory against any other KMI.

## Build

```sh
make -C /path/to/kernel/build M="$PWD" modules      # produces shadow_overlay2.ko
# or
make KDIR=/path/to/kernel/build
```

Build inside the `ghcr.io/ylarod/ddk-min:android14-6.1-<release>` DDK image
against its real `vmlinux`/`Module.symvers`.

## Verifying it loaded

```sh
insmod shadow_overlay2.ko
cat /proc/filesystems | grep overlay
```

## Licensing

`fs/overlayfs` is GPL-2.0-only kernel code by Miklos Szeredi and other Linux
kernel contributors (see the `SPDX-License-Identifier` and copyright headers
carried unmodified in each `.c`/`.h` file under `overlay/`).
