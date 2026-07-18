# overlay/ — vendored overlay filesystem subsystem

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

`../config.template` already spoofs `CONFIG_OVERLAY_FS=y` in the served
`/proc/config.gz` for the benefit of `dockerd`'s config preflight check — but,
exactly as documented in that template and in
[`../shadow_configspoof.README.md`](../shadow_configspoof.README.md),
spoofing the config text alone doesn't provide the behaviour, and turns a
clear "unsupported" preflight failure into the confusing runtime mount
failure above. This subsystem is the real, working code that backs that
spoofed config entry.

## What this is (and isn't)

Unlike the other `shadow_*` subsystems linked into `shadow_ctr.ko`, this is
**not** a reimplementation/simulation — it's the real, unmodified
`fs/overlayfs` sources from the `android14-6.1` branch of
https://android.googlesource.com/kernel/common, with only `module_init()`/
`module_exit()` replaced by the plain `shadow_overlay_init()`/
`shadow_overlay_exit()` functions that `shadow_ctr_main.c` calls (see
`shadow_ctr_internal.h`), and the per-file `MODULE_AUTHOR()`/
`MODULE_DESCRIPTION()`/`MODULE_LICENSE()` macros dropped (that metadata is
carried once, by `shadow_ctr_main.c`, like every other subsystem).

The only functional change relative to upstream `fs/overlayfs` is the same
compatibility fix already carried by
[`../../a14-6.1/`](../../a14-6.1)'s
`overlayfs_dont_make_DCACHE_OP_HASH_and_DCACHE_OP_COMPARE_weird.patch`
(dropping the `DCACHE_OP_HASH`/`DCACHE_OP_COMPARE` bits from
`ovl_dentry_weird()` in `util.c`, required for overlayfs to work on top of
case-insensitive filesystems as used by modern Android).

## Kernel version scope

This subsystem is vendored from, and only builds cleanly against,
**android14-6.1** (`6.1.y`). Overlayfs's internal VFS ABI (`vfs_tmpfile_open`,
`lookup_one`, `mnt_idmap` vs `user_namespace` parameters, `struct renamedata`
fields, etc.) changed across kernel versions, so the same source does not
compile unmodified against the other `ctr_patches/` branches
(`a12-5.10`, `a13-5.10`, `a13-5.15`, `a14-5.15`, `a15-6.6`, `a16-6.12`);
porting to those would need a per-branch source snapshot of `fs/overlayfs`
the same way `ctr_patches/` already splits its containerd patches by
Android/kernel version.

Because of this, unlike the other subsystems (which build across every
`ctr_patches/` branch via `shadow_ctr_internal.h`'s compat shims), this
subsystem is **not** unconditionally linked into `shadow_ctr.ko`. The parent
`Makefile`'s `WITH_SHADOW_OVERLAY` variable (default `1`) controls whether
`overlay/*.c` is compiled in and `SHADOW_CTR_WITH_OVERLAY` is defined; builds
targeting any branch other than android14-6.1 must pass
`WITH_SHADOW_OVERLAY=0`. See `../README.md` and `../Makefile` for details.

## Verifying it loaded

```sh
cat /proc/filesystems | grep overlay
```

## Licensing

`fs/overlayfs` is GPL-2.0-only kernel code by Miklos Szeredi and other Linux
kernel contributors (see the `SPDX-License-Identifier` and copyright headers
carried unmodified in each `.c`/`.h` file in this directory).
