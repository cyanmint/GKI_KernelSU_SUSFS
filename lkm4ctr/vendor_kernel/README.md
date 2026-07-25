# vendor_kernel

vendor_kernel is a parallel, vendored copy of the kernel namespace subsystem for `lkm4ctr`. Unlike `shadow_ns`, it hooks namespace syscalls unconditionally and installs a real, vendored `struct nsproxy *` directly on `task_struct->nsproxy` (via `vns_switch_task_namespaces()`), so the rest of the kernel (hostname, `/proc`, ipc/netns lookups, future `fork()`s) transparently observes the new namespace instead of the caller's original one.

## Real isolation vs. bookkeeping

- `unshare(CLONE_NEWxxx)` builds the new namespaces and then calls `vns_switch_task_namespaces(current, new_nsp)` to install them on the calling task for real. Because this happens synchronously before the syscall returns, any subsequent `fork()`/`clone()` from that task allocates its child's `struct pid` from the (now current) vendored `pid_ns_for_children`, giving real vpid remapping with no further glue code needed.
- `clone(CLONE_NEWxxx, ...)` (namespaces requested directly at clone time, without a prior `unshare()`) has the vns_* flags masked off before the underlying `clone()`/`clone3()` syscall runs, then the new namespaces are built and installed on the just-created child task. This makes UTS/IPC/USER/NET/MNT/CGROUP isolation real for that pattern too. The one caveat: because the real `copy_process()` already allocated the child's own `struct pid` from the *parent's* pid namespace before this hook runs, the child's own pid is not renumbered by this path (only namespaces it creates for its own descendants are new) — fully remapping the child's own pid for direct `clone(CLONE_NEWPID, ...)` would require hooking `copy_process()`/`kernel_clone()` itself.
- `setns(2)` (`vns_sys_setns()`) already performed a real install via the same switch primitive and required no changes.
- The per-tgid registry (`vns_task_find()` / `struct vns_task`) is retained purely for diagfs statistics (`stat_unshare`/`stat_setns`/`stat_clone`); it is no longer the source of truth for which namespaces a task is in — `task_struct->nsproxy` is.

## Slab-cache consistency with the real kernel

Once vendored namespaces are installed on the real `task_struct->nsproxy`/`cred->user_ns`, the real kernel's own exit path (`do_exit` -> `exit_task_namespaces` -> `free_nsproxy` -> `free_uts_ns`/`put_pid_ns`/`__put_user_ns`) eventually frees them, using `kmem_cache_free()` against the real kernel's private, non-exported `kmem_cache` instances (`uts_ns_cache`, `nsproxy_cachep`, `pid_ns_cachep`, `user_ns_cachep`). If those objects had been allocated with plain `kzalloc()`, SLUB's `cache_from_obj()` detects the mismatch ("Wrong slab cache") and the resulting corruption crashes the kernel (observed as a `kernel BUG at pid_namespace.h:76` panic on task exit).

To fix this, the 4 real cache pointers are resolved at init time via `shadow_hook_resolve()` (same mechanism already used for `init_cgroup_ns`/`init_ipc_ns`) into `vns_uts_ns_cache`/`vns_nsproxy_cachep`/`vns_pid_ns_cachep`/`vns_user_ns_cachep` (`glue/vendor_kernel_compat.c`), and `create_uts_ns()`/`create_nsproxy()`/`create_pid_namespace()`/`alloc_user_ns` in the corresponding vendored files now allocate/free through `kmem_cache_alloc()`/`kmem_cache_zalloc()`/`kmem_cache_free()` against these real caches instead of `kzalloc()`/`kfree()`, matching upstream's alloc-vs-zalloc semantics exactly. `vns_compat_ready()` fails closed (module init aborts) if any of the 4 caches cannot be resolved. `ipc_namespace`, `cgroup_namespace`, and `time_namespace` are unaffected — upstream itself allocates those with plain `kzalloc()`/`kmalloc()` + `kfree()`, so there is no cache mismatch to fix. The per-level `struct pid` slab cache (`ns->pid_cachep`, created via `create_pid_cachep()`) was already a real, self-consistent `kmem_cache_create()`-based cache and needed no change.

## Required kernel Kconfig options

`vendor_kernel` resolves several of the running kernel's namespace-private
symbols (e.g. `pid_ns_cachep`, `init_ipc_ns`) by name at module load time via
`shadow_hook_resolve()`. Historically, for any namespace type whose backing
`CONFIG_*_NS` option was not built into the running kernel, the corresponding
kernel object never existed to resolve, and `unshare(2)`/`clone(2)`/`setns(2)`
for that namespace type either fell back to bookkeeping-only behaviour or
failed with `-EINVAL` (this was the root cause of `ns_pid`/`ns_ipc` unshare
test failures seen on kernels that ship with `CONFIG_PID_NS=n`/`CONFIG_IPC_NS=n`,
even though `vendor_kernel` itself loaded and activated successfully).

`lkm4ctr/Makefile` now forces every one of the options below to `=1` directly
on the compiler command line for every vendor_kernel object file, so
`vendor_kernel`'s own code always compiles as if the running kernel had been
built with:

- `CONFIG_NAMESPACES=y`
- `CONFIG_UTS_NS=y`
- `CONFIG_IPC_NS=y` (also requires `CONFIG_SYSVIPC=y` and/or `CONFIG_POSIX_MQUEUE=y`, since `IPC_NS depends on (SYSVIPC || POSIX_MQUEUE)`)
- `CONFIG_PID_NS=y`
- `CONFIG_USER_NS=y`
- `CONFIG_NET_NS=y`
- `CONFIG_CGROUPS=y`
- `CONFIG_TIME_NS=y`

regardless of what the target kernel's actual `.config` says, so `unshare(2)`/
`clone(2)`/`setns(2)` for every namespace type vendored here are always
compiled in and never spuriously fail closed with `-EINVAL` due to the host
kernel's own build configuration. This is safe because none of these options
change the layout of any struct `vendor_kernel` touches (`struct nsproxy`'s
and `struct cgroup_namespace`'s member fields are unconditional upstream
regardless of these options) -- they only gate *declarations* of the real
kernel's own namespace symbols/functions, which `vendor_kernel` already
resolves defensively at runtime via `shadow_hook_resolve()` and fails closed
(`vns_compat_ready()`) if a resolution required for correct operation is
missing on that particular running kernel binary.

`lkm4ctr/Kconfig`'s `LKM4CTR_VENDOR_KERNEL` option also `select`s all of the
above, so any future in-tree build sourcing that file picks up matching
Kconfig-level documentation, but that file is never actually sourced by a
real Kconfig tree for this standalone out-of-tree build (see the comment
atop `lkm4ctr/Kconfig`) -- the `lkm4ctr/Makefile` compiler-flag forcing
described above is what actually takes effect for the module this
repository builds.

## Known remaining gaps

- POSIX message queues (`ipc/mqueue.c`) and SysV IPC (`ipc/msg.c`, `ipc/sem.c`, `ipc/shm.c`, `ipc/util.c`) are vendored sources but are not yet wired into `lkm4ctr/Makefile` or hooked to their syscalls; `mq_open()`/`msgget()`/etc. still resolve to `sys_ni_syscall()` on kernels built without `CONFIG_POSIX_MQUEUE`/`CONFIG_SYSVIPC`. Wiring these up needs dedicated syscall-hook glue (mirroring `vendor_kernel_syscalls.c`) plus Makefile changes.
- overlayfs upper/work directory cloning failures are an in-tree overlayfs behavior on the stock vendor kernel binary; `vendor_kernel` (an out-of-tree LKM) cannot patch code that is already compiled into the running kernel, so this is out of scope for this module.

## Vendoring rules

- upstream sources were copied from `kernel-common` `android14-6.1` (kernel `6.1.124`)
- all non-static global symbols are renamed with a `vns_` prefix
- slab-cache users for `ipc_namespace`/`cgroup_namespace`/`time_namespace` are `kzalloc`/`kfree` (matches upstream, which also uses plain kzalloc/kmalloc for these); `uts_namespace`/`nsproxy`/`pid_namespace`/`user_namespace` allocate/free through the real kernel's resolved `kmem_cache` pointers (see "Slab-cache consistency with the real kernel" above), since those structs are installed on the real `task_struct` and freed by the real kernel's exit path
- inode-number allocation uses `vns_alloc_inum()` / `vns_free_inum()`
- diagfs statistics are emitted through `vendor_kernel_diag_snprintf()`

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

- `vendor_kernel.h` - shared internal declarations
- `glue/vendor_kernel_module.c` - lifecycle, symbol resolution, registry
- `glue/vendor_kernel_syscalls.c` - syscall hooks; installs real namespaces via `vns_switch_task_namespaces()`
- `glue/vendor_kernel_diag.c` - diagfs renderer
- `include/uapi/vendor_kernel.h` - minimal UAPI marker header

## Diffing against upstream

Run:

```sh
./vendor_kernel_diff.sh [path-to-kernel-common]
```

With no argument it defaults to `$RUNNER_TEMP/kernel-common`.

