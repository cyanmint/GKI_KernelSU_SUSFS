# shadow_ctr — combined containerd-support kernel module

`shadow_ctr.ko` is a single out-of-tree kernel module that links together five
independently documented subsystems, each of which used to be its own
standalone module:

| Subsystem            | Source                  | Docs                                            |
|-----------------------|--------------------------|--------------------------------------------------|
| Namespaces            | `shadow_ns.c`             | [`shadow_ns.README.md`](shadow_ns.README.md)             |
| System V IPC          | `shadow_sysvipc.c`        | [`shadow_sysvipc.README.md`](shadow_sysvipc.README.md)   |
| POSIX message queues  | `shadow_mqueue.c`         | [`shadow_mqueue.README.md`](shadow_mqueue.README.md)     |
| cgroup device control | `shadow_cgdevices.c`      | [`shadow_cgdevices.README.md`](shadow_cgdevices.README.md) |
| `/proc/config.gz` spoof | `shadow_configspoof.c` | [`shadow_configspoof.README.md`](shadow_configspoof.README.md) |
| ftrace/kprobe hijack helper | `shadow_hook.h`     | [`shadow_hook.README.md`](shadow_hook.README.md)         |

`shadow_ctr_main.c` only carries the combined module's metadata/banner; each
subsystem exposes plain (non-`module_init`/`module_exit`) init/exit functions
declared in `shadow_ctr_internal.h` that `shadow_ctr_main.c` calls explicitly,
in order, with rollback on failure (the module loader only allows a single
`init_module()`/`cleanup_module()` alias per `.ko`, so only one file may use
the `module_init()`/`module_exit()` macros).

## Why merge them

Each subsystem simulates a piece of GKI kernel functionality
(`CONFIG_*_NS`, `CONFIG_SYSVIPC`, `CONFIG_POSIX_MQUEUE`, cgroup-v1 device
control, and the corresponding `/proc/config.gz` advertisement) for kernels
built without it, so that a stock, unpatched `containerd`/`runc`/`dockerd` can
run on them. Building and loading a single `shadow_ctr.ko` is simpler to ship
and version than five separate `.ko` files that must be kept in lockstep.

## Building

Out-of-tree, against a prepared kernel source/build tree:

```sh
cd ctr_patches/shadow_ctr
make KDIR=/path/to/kernel/build
sudo insmod shadow_ctr.ko
```

For an Android GKI cross build:

```sh
make KDIR=/path/to/kernel/build ARCH=arm64 LLVM=1
```

See each subsystem's own README (linked above) for its specific `/dev/*` node,
ioctl ABI, and honest scope/limitations. See
[`../../.github/workflows/build-shadow-ctr.yml`](../../.github/workflows/build-shadow-ctr.yml)
for a CI workflow that builds `shadow_ctr.ko` against a matrix of real
`kernel/common` branches.

## Kernel compatibility

`shadow_hook.h` picks its hooking backend at compile time: ftrace-based when
`CONFIG_FUNCTION_TRACER`/`CONFIG_DYNAMIC_FTRACE` are available, and a
kprobe-`pre_handler` fallback otherwise — the latter is required on stock
Android GKI kernels, which ship with `CONFIG_FUNCTION_TRACER` disabled.

Some subsystems (e.g. `shadow_mqueue.c`'s `fd_file()`/`fd_empty()` use) target
kernel APIs that only exist from Linux v6.8 onward; `shadow_ctr_internal.h`
provides compatibility shims so the same source builds unmodified against
older GKI kernel branches (e.g. 6.1) that predate those helpers.
