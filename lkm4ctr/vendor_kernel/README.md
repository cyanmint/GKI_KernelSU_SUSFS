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

## Namespace refcounting is fully self-contained (PID_NS / USER_NS)

`get_pid_ns()`/`put_pid_ns()` (`include/linux/pid_namespace.h`) and
`get_user_ns()`/`put_user_ns()`/`__put_user_ns()`
(`include/linux/user_namespace.h`) are declared differently depending on the
*target* kernel's own `CONFIG_PID_NS`/`CONFIG_USER_NS` setting: a real,
exported function (or a real `refcount_inc()`/`refcount_dec_and_test()`
body) when the option is `y`, versus a no-op `static inline` stub when it is
`n`. When these options are `n`, `put_pid_ns()` and `__put_user_ns()` are
not merely unexported -- they are entirely absent from vmlinux, so no
runtime symbol resolution (`shadow_hook_resolve()`) can ever find them.
Likewise, several `#ifdef CONFIG_PID_NS`/`#ifdef CONFIG_USER_NS` blocks in
upstream's `kernel/nsproxy.c` (`validate_nsset()`/`commit_nsset()`, used by
`setns(2)`) are compiled out entirely when the target lacks these options,
which would silently skip pid/user namespace installation during `setns(2)`
even though `vendor_kernel` fully implements both namespace types itself.

To make `vendor_kernel` install, refcount, and free its own `pid_namespace`/
`user_namespace` objects correctly regardless of the target kernel's
`CONFIG_PID_NS`/`CONFIG_USER_NS` setting, every vendored call site uses
local, unconditionally-compiled equivalents instead of calling the real
kernel functions or gating on these macros:

- `vns_get_pid_ns()` / `vns_put_pid_ns()` (`kernel/pid_namespace.c`) replace
  `get_pid_ns()`/`put_pid_ns()`.
- `vns_get_user_ns()` / `vns_put_user_ns()` (`kernel/user_namespace.c`)
  replace `get_user_ns()`/`put_user_ns()`, and `vns_put_user_ns()` itself
  routes to `vns___put_user_ns()` (the already-vendored teardown function)
  on the final put.
- The `#ifdef CONFIG_PID_NS`/`#ifdef CONFIG_USER_NS` blocks in
  `vns_sys_setns()`'s `validate_nsset()`/`commit_nsset()` helpers
  (`kernel/nsproxy.c`) were removed (made unconditional), since
  `vendor_kernel` always vendors both namespace types itself.

These shims reuse the same `ns.count`/`kref.refcount` field that the real
kernel functions operate on (via the existing `vns_pid_get_ref`/
`vns_pid_put_ref`/`vns_user_get_ref`/`vns_user_put_ref` macros in
`vendor_kernel.h`, which are already version-gated for the pre-5.15
`kref`-based layout vs. the 5.15+ `ns_common.count` layout), so refcounting
stays correct and slab-cache-consistent (see "Slab-cache consistency with
the real kernel" above) no matter what the target kernel's own
`CONFIG_PID_NS`/`CONFIG_USER_NS` says.

**Do not instead force these `CONFIG_*` options to `=1` at compile time
(e.g. via `-DCONFIG_PID_NS=1`/`-DCONFIG_USER_NS=1` compiler flags) to work
around a target kernel that lacks them.** Forcing the macro to `1` at
compile time makes vendor_kernel's compiled code take the `extern` branch
of the *kernel's own* declarations (e.g. `from_kuid()`/`from_kgid()` in
`include/linux/uidgid.h`) and reference the real exported symbol
unconditionally; if the actual running kernel was truly built with that
option `=n`, the symbol is genuinely absent there (not just unexported),
and `insmod` fails with `Unknown symbol from_kuid`/`put_pid_ns`/
`__put_user_ns`/etc. (err -2). This was tried once and reverted after
reproducing exactly this failure -- the local-shim approach above is the
supported fix instead.

## Required kernel Kconfig options

`vendor_kernel` resolves several of the running kernel's namespace-private
symbols (e.g. `pid_ns_cachep`, `init_ipc_ns`) by name at module load time via
`shadow_hook_resolve()`. For any namespace type whose backing `CONFIG_*_NS`
option is not built into the running kernel, the corresponding kernel object
never exists to resolve, and `unshare(2)`/`clone(2)`/`setns(2)` for that
namespace type either falls back to bookkeeping-only behaviour or fails with
`-EINVAL` (this is the root cause of `ns_ipc`/`ns_net` unshare test failures
seen on kernels that ship with `CONFIG_IPC_NS=n`/`CONFIG_NET_NS=n`, even
though `vendor_kernel` itself loads and activates successfully). `PID_NS` and
`USER_NS` are the exception: as documented above under "Namespace
refcounting is fully self-contained", vendor_kernel no longer depends on the
target's `CONFIG_PID_NS`/`CONFIG_USER_NS` at all. The running kernel still
must be built with the remaining options for full namespace coverage:

- `CONFIG_NAMESPACES=y`
- `CONFIG_UTS_NS=y`
- `CONFIG_IPC_NS=y` (also requires `CONFIG_SYSVIPC=y` and/or `CONFIG_POSIX_MQUEUE=y`, since `IPC_NS depends on (SYSVIPC || POSIX_MQUEUE)`)
- `CONFIG_NET_NS=y`
- `CONFIG_CGROUPS=y`
- `CONFIG_TIME_NS=y`

`lkm4ctr/Kconfig`'s `LKM4CTR_VENDOR_KERNEL` option `select`s `CONFIG_PID_NS`
and `CONFIG_USER_NS` too (along with the rest), so any future in-tree build
sourcing that file still forces them on for consistency and for any code
elsewhere in the kernel that assumes them, but `vendor_kernel` itself does
not require it any more for its own pid/user namespace support.

## Known remaining gaps

- POSIX message queues (`ipc/mqueue.c`) and SysV IPC (`ipc/msg.c`, `ipc/sem.c`, `ipc/shm.c`, `ipc/util.c`) are vendored sources but are not yet wired into `lkm4ctr/Makefile` or hooked to their syscalls; `mq_open()`/`msgget()`/etc. still resolve to `sys_ni_syscall()` on kernels built without `CONFIG_POSIX_MQUEUE`/`CONFIG_SYSVIPC`. Wiring these up needs dedicated syscall-hook glue (mirroring `vendor_kernel_syscalls.c`) plus Makefile changes. These files are unaffected by the `CONFIG_PID_NS`/`CONFIG_USER_NS` self-containment above since they are not currently compiled into `lkm4ctr.ko`.
- `NET_NS` and `MNT_NS` are still resolved via optional function pointers (`vns_copy_net_ns_fn`/`vns_copy_mnt_ns_fn` in `glue/vendor_kernel_module.c`) rather than being fully vendored, so they still silently fall back to bookkeeping-only/no-op behaviour on a target kernel with `CONFIG_NET_NS=n`/`CONFIG_NAMESPACES` MNT support missing.
- `kernel/cgroup/namespace.c` and `kernel/time/namespace.c` store `user_ns` without taking a reference on it (`(void)user_ns;` in their copy functions), unlike `kernel/pid_namespace.c`/`kernel/user_namespace.c`/`kernel/utsname.c`/`ipc/namespace.c` which now use `vns_get_user_ns()`/`vns_put_user_ns()`; this is a pre-existing gap unrelated to `CONFIG_PID_NS`/`CONFIG_USER_NS` and was not changed here.
- overlayfs upper/work directory cloning failures are an in-tree overlayfs behavior on the stock vendor kernel binary; `vendor_kernel` (an out-of-tree LKM) cannot patch code that is already compiled into the running kernel, so this is out of scope for this module.

## Vendoring rules

- upstream sources were copied from `kernel-common` `android14-6.1` (kernel `6.1.124`)
- all non-static global symbols are renamed with a `vns_` prefix
- slab-cache users for `ipc_namespace`/`cgroup_namespace`/`time_namespace` are `kzalloc`/`kfree` (matches upstream, which also uses plain kzalloc/kmalloc for these); `uts_namespace`/`nsproxy`/`pid_namespace`/`user_namespace` allocate/free through the real kernel's resolved `kmem_cache` pointers (see "Slab-cache consistency with the real kernel" above), since those structs are installed on the real `task_struct` and freed by the real kernel's exit path
- `get_pid_ns()`/`put_pid_ns()`/`get_user_ns()`/`put_user_ns()` call sites are replaced with `vns_get_pid_ns()`/`vns_put_pid_ns()`/`vns_get_user_ns()`/`vns_put_user_ns()` (see "Namespace refcounting is fully self-contained" above), and the `#ifdef CONFIG_PID_NS`/`#ifdef CONFIG_USER_NS` blocks gating pid/user namespace installation in `vns_sys_setns()`'s helpers are made unconditional, for the same reason
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

