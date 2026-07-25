# vendor_ns

vendor_ns is a parallel, vendored copy of the kernel namespace subsystem for `lkm4ctr`. Unlike `shadow_ns`, it hooks namespace syscalls unconditionally and tracks a vendored `struct nsproxy *` per task-group (`tgid -> vns_task`).

## Vendoring rules

- upstream sources were copied from `kernel-common` `android14-6.1` (kernel `6.1.124`)
- all non-static global symbols are renamed with a `vns_` prefix
- slab-cache users are converted to `kzalloc`/`kfree` so the code can build out-of-tree
- inode-number allocation uses `vns_alloc_inum()` / `vns_free_inum()`
- diagfs statistics are emitted through `vendor_ns_diag_snprintf()`

## Vendored files

- `kernel/utsname.c`
- `kernel/nsproxy.c`
- `kernel/pid_namespace.c`
- `kernel/user_namespace.c`
- `ipc/namespace.c`
- `fs/nsfs.c`
- `kernel/cgroup/namespace.c`
- `kernel/time/namespace.c`

## Helper files

- `vendor_ns.h` - shared internal declarations
- `glue/vendor_ns_module.c` - lifecycle, symbol resolution, registry
- `glue/vendor_ns_syscalls.c` - syscall hooks
- `glue/vendor_ns_diag.c` - diagfs renderer
- `include/uapi/vendor_ns.h` - minimal UAPI marker header

## Diffing against upstream

Run:

```sh
./vendor_ns_diff.sh [path-to-kernel-common]
```

With no argument it defaults to `$RUNNER_TEMP/kernel-common`.
