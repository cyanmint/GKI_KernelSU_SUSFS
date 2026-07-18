# shadow_overlay — vendored overlay filesystem module

## Why this exists

`dockerd`'s `overlay2` storage driver (and `containerd`/`runc`) needs the
kernel to actually register an `"overlay"` filesystem type; it does not
merely check `/proc/config.gz`. Stock/production Android GKI boot images are
generally built with `CONFIG_OVERLAY_FS` disabled entirely (no `overlay.ko`
either), so `overlay2` fails to initialize with an error such as:

```
failed to mount overlay: invalid argument
error creating overlay mount to /var/lib/docker/overlay2/.../merged: invalid argument
```

Note that `ctr_patches/shadow_ctr/config.template` already spoofs
`CONFIG_OVERLAY_FS=y` in the served `/proc/config.gz` for the benefit of
`dockerd`'s config preflight check — but, exactly as documented in that
template, spoofing the config text alone doesn't provide the behaviour, and
turns a clear "unsupported" preflight failure into the confusing runtime
mount failure above. `shadow_overlay` is the real, working module that
backs that spoofed config entry.

## What this is (and isn't)

Unlike the other `ctr_patches/shadow_*` modules, this is **not** a
reimplementation/simulation — it's an out-of-tree packaging of the real,
unmodified `fs/overlayfs` sources from the
`android14-6.1` branch of https://android.googlesource.com/kernel/common,
built as a standalone `overlay.ko` that can be `insmod`'d into a running
kernel that never built overlayfs in at all.

The only change relative to upstream `fs/overlayfs` is the same
compatibility fix already carried by
[`../a14-6.1/`](../a14-6.1)'s
`overlayfs_dont_make_DCACHE_OP_HASH_and_DCACHE_OP_COMPARE_weird.patch`
(dropping the `DCACHE_OP_HASH`/`DCACHE_OP_COMPARE` bits from
`ovl_dentry_weird()`, required for overlayfs to work on top of case-
insensitive filesystems as used by modern Android).

## Kernel version scope

This module is vendored from, and only builds/loads cleanly against,
**android14-6.1** (`6.1.y`). Overlayfs's internal VFS ABI (`vfs_tmpfile_open`,
`lookup_one`, `mnt_idmap` vs `user_namespace` parameters, `struct renamedata`
fields, etc.) changed across kernel versions, so the same source does not
compile unmodified against the other `ctr_patches/` branches
(`a12-5.10`, `a13-5.10`, `a13-5.15`, `a14-5.15`, `a15-6.6`, `a16-6.12`);
porting to those would need a per-branch source snapshot of
`fs/overlayfs` the same way `ctr_patches/` already splits its containerd
patches by Android/kernel version.

## Building

Out-of-tree, against a prepared kernel source/build tree (needs a real
`vmlinux`/`Module.symvers` for the matching KMI — see
[`../shadow_ctr/README.md`](../shadow_ctr/README.md)'s note on why a bare
`gki_defconfig` + `modules_prepare` tree is not sufficient):

```sh
cd ctr_patches/shadow_overlay
make KDIR=/path/to/android14-6.1/kernel/build
sudo insmod overlay.ko
```

For an Android GKI cross build:

```sh
make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1
```

Verify it registered correctly:

```sh
cat /proc/filesystems | grep overlay
```

See
[`../../.github/workflows/build-shadow-overlay.yml`](../../.github/workflows/build-shadow-overlay.yml)
for a CI workflow that builds `overlay.ko` against the android14-6.1 DDK
image.

## Loading together with the other shadow_* modules

`shadow_overlay` is independent of `shadow_ctr.ko` (it doesn't call into or
depend on any of the `shadow_*` subsystems) and can be loaded before or
after it:

```sh
sudo insmod overlay.ko
sudo insmod ../shadow_ctr/shadow_ctr.ko
```

## Licensing

`fs/overlayfs` is GPL-2.0-only kernel code by Miklos Szeredi and other Linux
kernel contributors (see the `SPDX-License-Identifier` and copyright headers
carried unmodified in each `.c`/`.h` file in this directory); this directory
only adds the out-of-tree `Makefile` wrapper.
