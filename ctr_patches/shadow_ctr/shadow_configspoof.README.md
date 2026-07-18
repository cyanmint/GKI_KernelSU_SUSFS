# shadow_configspoof — /proc/config.gz spoofing overlay

`shadow_configspoof` is one of the subsystems linked into the combined
`shadow_ctr.ko` module (see `README.md` in this directory for the umbrella
overview). It overlays `/proc/config.gz` with a build-time-generated,
gzip-compressed configuration text advertising the `CONFIG_*` symbols
container runtimes probe for
(`CONFIG_NAMESPACES`, `CONFIG_SYSVIPC`, `CONFIG_POSIX_MQUEUE`,
`CONFIG_CGROUP_DEVICE`, bridge/netfilter symbols, ...) as `y`, regardless of
what `vmlinux` was actually built with.

## Why this exists

`dockerd`'s `contrib/check-config.sh`, containerd's CRI feature probing, and
various `docker info` preflight warnings parse `/proc/config.gz` (or
`/boot/config-$(uname -r)`) and refuse to start, or print scary warnings and
disable features, when expected `CONFIG_*` symbols are missing. On a GKI
kernel built without several of those options, those preflight checks fail
even though the [`shadow_hook`](shadow_hook.README.md)-based `shadow_ns` /
`shadow_sysvipc` / `shadow_mqueue` / `shadow_cgdevices` subsystems already
transparently provide (partial, honestly-documented) behaviour for the
corresponding subsystem.

`shadow_configspoof` makes the kernel's self-report to userspace consistent with
what those subsystems actually provide, so unconditional `CONFIG_*` preflight
checks stop being a hard blocker for stock containerd/dockerd.

## What it does *not* do

**It only spoofs the advertisement.** It does not change any kernel
behaviour by itself. Only load `shadow_ctr.ko` together with enabling the
shadow subsystem(s) that back the `CONFIG_*` symbol(s) you are spoofing — e.g.
spoofing `CONFIG_SYSVIPC=y` without `shadow_sysvipc` active just turns a clear
"container refused to start" failure at preflight time into a confusing
runtime `-ENOSYS` once the container actually calls `msgget(2)`.

Note that even with the corresponding subsystem active, some of the spoofed
symbols only have *partial* fidelity (see each subsystem's own README's
"Honest scope / limitations" section, e.g. `shmat`/`shmdt` in `shadow_sysvipc`
remain unimplemented, `shadow_cgdevices` enforcement requires an explicit bind
step). Spoofing the config is a userspace-preflight compatibility shim, not
a claim that every corresponding kernel subsystem is now fully implemented.

`CONFIG_OVERLAY_FS` is the one spoofed symbol *not* backed by a subsystem
linked into `shadow_ctr.ko` itself — it is instead backed by the sibling,
separately-loaded [`../shadow_overlay/`](../shadow_overlay/README.md)
module (`overlay.ko`), which is a vendored, unmodified copy of the real
`fs/overlayfs` kernel code rather than a simulation. Load it alongside
`shadow_ctr.ko` on any target where `CONFIG_OVERLAY_FS=y` is spoofed here,
or dockerd's `overlay2` graphdriver will fail at mount time instead of at
the config preflight check.

## Implementation

The exact gzip byte stream is generated at **build time** from
`config.template` by `gen_config_gz.py` into a generated `config_gz_data.h`
(a plain C byte array), rather than compressed at module-load time — this
means the module has **no runtime dependency on `CONFIG_ZLIB_DEFLATE`** being
enabled in the target kernel.

At `module_init()`:
1. Any pre-existing `/proc/config.gz` (the in-tree `CONFIG_IKCONFIG_PROC`
   implementation, if present) is removed.
2. A new read-only `/proc/config.gz` `proc_dir_entry` is created, whose
   `.proc_read` just hands the embedded buffer to userspace via
   `simple_read_from_buffer()`.

At `module_exit()` the entry is removed. The module does **not** attempt to
restore a pre-existing native ikconfig entry on unload — there is no way to
recover its original `proc_dir_entry` handle from a separate module, so
unloading `shadow_ctr` simply leaves `/proc/config.gz` absent, matching
the many GKI configs that already ship `CONFIG_IKCONFIG_PROC` disabled.

## Customizing the spoofed config

Edit `config.template` (a real, plain-text `.config`-style file) and rebuild;
`gen_config_gz.py` regenerates `config_gz_data.h` automatically as part of
`make`.

## Files

| Path                  | Purpose                                                |
|------------------------|--------------------------------------------------------|
| `config.template`      | Plaintext spoofed kernel config, edited by maintainers. |
| `gen_config_gz.py`     | Build-time generator: gzip-compresses the template into a C byte array. |
| `config_gz_data.h`     | Generated (gitignored) — the embedded gzip byte array.  |
| `shadow_configspoof.c` | This subsystem's source, linked into `shadow_ctr.ko`.   |
| `Makefile`             | Out-of-tree build for the combined module (`make KDIR=...`), also regenerates `config_gz_data.h`. |

## Building

```sh
cd ctr_patches/shadow_ctr
make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1
sudo insmod shadow_ctr.ko
zcat /proc/config.gz | grep CONFIG_SYSVIPC
```

## Loading order

`shadow_configspoof` is linked into `shadow_ctr.ko` alongside `shadow_ns` /
`shadow_sysvipc` / `shadow_mqueue` / `shadow_cgdevices`, so there is no
separate load order to manage between them — a single `insmod shadow_ctr.ko`
brings all subsystems up together.
