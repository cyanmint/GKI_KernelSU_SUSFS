# vendor_kernel

vendor_kernel is a parallel, vendored copy of the kernel namespace subsystem for `lkm4ctr`. Unlike `shadow_ns`, it hooks namespace syscalls unconditionally and installs a real, vendored `struct nsproxy *` directly on `task_struct->nsproxy` (via `vns_switch_task_namespaces()`), so the rest of the kernel (hostname, `/proc`, ipc/netns lookups, future `fork()`s) transparently observes the new namespace instead of the caller's original one.

## Real isolation vs. bookkeeping

- `unshare(CLONE_NEWxxx)` builds the new namespaces and then calls `vns_switch_task_namespaces(current, new_nsp)` to install them on the calling task for real. Because this happens synchronously before the syscall returns, any subsequent `fork()`/`clone()` from that task allocates its child's `struct pid` from the (now current) vendored `pid_ns_for_children`, giving real vpid remapping with no further glue code needed.
- `clone(CLONE_NEWxxx, ...)` (namespaces requested directly at clone time, without a prior `unshare()`) has the vns_* flags masked off before the underlying `clone()`/`clone3()` syscall runs, then the new namespaces are built and installed on the just-created child task. This makes UTS/IPC/USER/NET/MNT/CGROUP isolation real for that pattern too. The one caveat: because the real `copy_process()` already allocated the child's own `struct pid` from the *parent's* pid namespace before this hook runs, the child's own pid is not renumbered by this path (only namespaces it creates for its own descendants are new) — fully remapping the child's own pid for direct `clone(CLONE_NEWPID, ...)` would require hooking `copy_process()`/`kernel_clone()` itself.
- `setns(2)` (`vns_sys_setns()`) already performed a real install via the same switch primitive and required no changes.
- The per-tgid registry (`vns_task_find()` / `struct vns_task`) is retained purely for diagfs statistics (`stat_unshare`/`stat_setns`/`stat_clone`); it is no longer the source of truth for which namespaces a task is in — `task_struct->nsproxy` is.

## Known remaining gaps

- POSIX message queues (`ipc/mqueue.c`) and SysV IPC (`ipc/msg.c`, `ipc/sem.c`, `ipc/shm.c`, `ipc/util.c`) are vendored sources but are not yet wired into `lkm4ctr/Makefile` or hooked to their syscalls; `mq_open()`/`msgget()`/etc. still resolve to `sys_ni_syscall()` on kernels built without `CONFIG_POSIX_MQUEUE`/`CONFIG_SYSVIPC`. Wiring these up needs dedicated syscall-hook glue (mirroring `vendor_kernel_syscalls.c`) plus Makefile changes.
- overlayfs upper/work directory cloning failures are an in-tree overlayfs behavior on the stock vendor kernel binary; `vendor_kernel` (an out-of-tree LKM) cannot patch code that is already compiled into the running kernel, so this is out of scope for this module.

## Vendoring rules

- upstream sources were copied from `kernel-common` `android14-6.1` (kernel `6.1.124`)
- all non-static global symbols are renamed with a `vns_` prefix
- slab-cache users are converted to `kzalloc`/`kfree` so the code can build out-of-tree
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

