# vendor_kernel

vendor_kernel is a parallel, vendored copy of the kernel namespace subsystem for `lkm4ctr`. Unlike `shadow_ns`, it hooks namespace syscalls unconditionally and installs a real, vendored `struct nsproxy *` directly on `task_struct->nsproxy` (via `vns_switch_task_namespaces()`), so the rest of the kernel (hostname, `/proc`, ipc/netns lookups, future `fork()`s) transparently observes the new namespace instead of the caller's original one.

## Real isolation vs. bookkeeping

- `unshare(CLONE_NEWxxx)` builds the new namespaces and then calls `vns_switch_task_namespaces(current, new_nsp)` to install them on the calling task for real. Because this happens synchronously before the syscall returns, any subsequent `fork()`/`clone()` from that task sees the new namespace state immediately. `CLONE_NEWPID` is the one runtime-gated exception: vendor_kernel only installs a new `pid_ns_for_children` when the *running* kernel actually has its own pid-namespace core (`copy_pid_ns()` present). On a stock `CONFIG_PID_NS=n` kernel it now degrades to a no-op instead of letting the real exit path hit the inline `zap_pid_ns_processes()` BUG stub.
- `clone(CLONE_NEWxxx, ...)` (namespaces requested directly at clone time, without a prior `unshare()`) has the vns_* flags masked off before the underlying `clone()`/`clone3()` syscall runs, then the new namespaces are built and installed on the just-created child task. This makes UTS/IPC/USER/NET/MNT/CGROUP isolation real for that pattern too. The one caveat: because the real `copy_process()` already allocated the child's own `struct pid` from the *parent's* pid namespace before this hook runs, the child's own pid is not renumbered by this path (only namespaces it creates for its own descendants are new) — fully remapping the child's own pid for direct `clone(CLONE_NEWPID, ...)` would require hooking `copy_process()`/`kernel_clone()` itself.
- `setns(2)` (`vns_sys_setns()`) already performed a real install via the same switch primitive and required no changes.
- The per-tgid registry (`vns_task_find()` / `struct vns_task`) is retained purely for diagfs statistics (`stat_unshare`/`stat_setns`/`stat_clone`); it is no longer the source of truth for which namespaces a task is in — `task_struct->nsproxy` is.

## Slab-cache consistency with the real kernel

Once vendored namespaces are installed on the real `task_struct->nsproxy`/`cred->user_ns`, the real kernel's own exit path (`do_exit` -> `exit_task_namespaces` -> `free_nsproxy` -> `free_uts_ns`/`put_pid_ns`/`__put_user_ns`) would eventually try to free them via `kmem_cache_free()` against the real kernel's private, non-exported `kmem_cache` instances (`uts_ns_cache`, `nsproxy_cachep`, `pid_ns_cachep`, `user_ns_cachep`). If those objects had been allocated with plain `kzalloc()` -- or with the *real* kernel's cache, which then gets freed against a *different*, module-owned cache, or vice versa -- SLUB's `cache_from_obj()` detects the mismatch ("Wrong slab cache") and the resulting corruption crashes the kernel (observed as a `kernel BUG at pid_namespace.h:76` panic on task exit).

**Resolving the 4 real cache pointers by name is fundamentally unreliable on real devices and was abandoned.** An earlier revision resolved them at init time via `shadow_hook_resolve()` (which is itself `register_kprobe()`-based symbol lookup through kallsyms). That only ever works for kallsyms *function* symbols; `uts_ns_cache`/`nsproxy_cachep`/`pid_ns_cachep`/`user_ns_cachep` are all `static struct kmem_cache *` **data** variables, which kallsyms only exposes when the target kernel was built with `CONFIG_KALLSYMS_ALL` -- essentially never the case on production/certified/GKI Android kernels. Worse, `utsname.o`/`user_namespace.o`/`pid_namespace.o` are themselves gated by `obj-$(CONFIG_UTS_NS)`/`obj-$(CONFIG_USER_NS)`/`obj-$(CONFIG_PID_NS)` in upstream's `kernel/Makefile`, so on a target with those configs `=n` (`vendor_kernel`'s entire reason for existing), `uts_ns_cache`/`pid_ns_cachep`/`user_ns_cachep` don't even exist in vmlinib to resolve. This combination made the 4-cache resolution fail unconditionally on real hardware, and `vns_compat_ready()` then failed the whole module load with `-ENOENT` ("uts_ns_cache/nsproxy_cachep/pid_ns_cachep/user_ns_cachep unresolved; vendor_kernel unavailable").

The fix: **all 4 caches are now entirely module-owned**, created via `kmem_cache_create()` in each subsystem's own init function (`vns_uts_ns_init()`, `vns_nsproxy_cache_init()`, `vns_pid_ns_init()`, `vns_user_ns_init()`) instead of ever being resolved from the running kernel. `create_uts_ns()`/`create_nsproxy()`/`create_pid_namespace()`/`alloc_user_ns()` in the corresponding vendored files still allocate/free through `kmem_cache_alloc()`/`kmem_cache_zalloc()`/`kmem_cache_free()` (matching upstream's alloc-vs-zalloc semantics), just against vendor_kernel's own caches -- no call-site changes were needed for this. `vns_compat_ready()` no longer treats any of the 4 caches as part of its readiness gate; `vendor_kernel_init()` fails closed with `-ENOMEM` only if `kmem_cache_create()` itself fails (extremely unlikely).

Making the caches module-owned only solves half the problem: the real kernel's own exit path must now be kept from ever calling `kmem_cache_free()` against these module-owned objects at all, since it would use the wrong (real) cache pointer. `uts_namespace`/`pid_namespace`/`user_namespace` are safe by construction here: when the target's `CONFIG_UTS_NS`/`CONFIG_PID_NS`/`CONFIG_USER_NS` is `n` (the scenario vendor_kernel targets), the real kernel's own `put_uts_ns()`/`put_pid_ns()`/`__put_user_ns()` compile to no-ops (see "Namespace refcounting is fully self-contained" below), so the real exit path never reaches a real `kmem_cache_free()` call for these three types regardless. `nsproxy` itself is the one unconditional hazard: `kernel/nsproxy.c` is always `obj-y`, so the real kernel's `free_nsproxy()` *always* unconditionally calls `kmem_cache_free(nsproxy_cachep, ns)` — a guaranteed "wrong slab cache" panic if `ns` is one of vendor_kernel's own objects.

To close this, `kernel/nsproxy.c` adds:

- `vns_nsproxy_set` — a small spinlock-protected hash table (`kernel/nsproxy.c`) of every live, module-owned `struct nsproxy *` (inserted in `create_nsproxy()`, removed in `vns_free_nsproxy()`). A pointer-identity check, not a tgid/task lookup, so it works correctly regardless of how many threads/tasks share the pointer via ordinary `fork()`.
- `vns_task_exit_cleanup(struct task_struct *tsk)` — if `tsk->nsproxy` is currently module-owned (per `vns_nsproxy_set`), swaps it onto the pinned `vns_init_nsproxy` singleton (refcount pinned to a large sentinel at `vns_nsproxy_cache_init()` time so it can never reach zero) and tears the old, module-owned object down completely through vendor_kernel's own self-contained free path (`vns_put_nsproxy()`/`vns_free_nsproxy()`).
- A **plain `pre_handler`-only `struct kprobe` on `do_exit()`** (`vns_exit_hook_init()`/`vns_exit_hook_exit()`, `kernel/nsproxy.c`), registered from `vendor_kernel_init()`/removed from `vendor_kernel_exit()`, calls `vns_task_exit_cleanup(current)` for every exiting task on the system, strictly before the real `do_exit()` body (and therefore `exit_task_namespaces()`/`free_nsproxy()`) runs. By the time the real path executes, `tsk->nsproxy` already points at the pinned, never-slab-allocated `vns_init_nsproxy`, and the real kernel never touches a module-owned object. This is *not* implemented as one of the redirecting `SHADOW_HOOK()` entries used for syscalls elsewhere in `vendor_kernel`: `do_exit()` is `__noreturn`, so redirecting it would mean the rmmod-safety kretprobe placed on the *replacement* function (see `common/shadow_hook.h`'s "rmmod safety" note) could never fire on return, permanently pinning `module_refcount()` above zero the moment the first process on the whole system exits. A plain pre-handler kprobe has no such problem: it runs and returns normally, and `register_kprobe()`/`unregister_kprobe()` are already the same primitive `shadow_hook_resolve()` itself relies on.

**Known accepted remaining gap:** `cred->user_ns` can be freed independently of nsproxy teardown, via a deferred RCU callback (`put_cred_rcu()`) at an arbitrary later time and context, decoupled from task exit. A similarly rigorous fix would require intercepting `__put_user_ns()` or the cred RCU-free path itself, which is materially harder to make safe and is out of scope here. This is a plain no-op leak (not a crash risk) whenever the target's `CONFIG_USER_NS=n` (vendor_kernel's primary use case); it is only a real hazard in the unusual combination where the target ships `CONFIG_USER_NS=y` while still lacking other namespace types vendor_kernel exists for.

`ipc_namespace`, `cgroup_namespace`, and `time_namespace` are unaffected — upstream itself allocates those with plain `kzalloc()`/`kmalloc()` + `kfree()`, so there is no cache mismatch to fix. The per-level `struct pid` slab cache (`ns->pid_cachep`, created via `create_pid_cachep()`) was already a real, self-consistent `kmem_cache_create()`-based cache and needed no change.

## Namespace refcounting is fully self-contained (UTS_NS / PID_NS / USER_NS)

`get_uts_ns()`/`put_uts_ns()` (`include/linux/utsname.h`),
`get_pid_ns()`/`put_pid_ns()` (`include/linux/pid_namespace.h`) and
`get_user_ns()`/`put_user_ns()`/`__put_user_ns()`
(`include/linux/user_namespace.h`) are declared differently depending on the
*target* kernel's own `CONFIG_UTS_NS`/`CONFIG_PID_NS`/`CONFIG_USER_NS`
setting: a real, exported function (or a real `refcount_inc()`/
`refcount_dec_and_test()` body) when the option is `y`, versus a no-op
`static inline` stub when it is `n`. When these options are `n`,
`put_uts_ns()`/`put_pid_ns()`/`__put_user_ns()` are not merely unexported --
they are entirely absent from vmlinux, so no runtime symbol resolution
(`shadow_hook_resolve()`) can ever find them.
Likewise, several `#ifdef CONFIG_PID_NS`/`#ifdef CONFIG_USER_NS` blocks in
upstream's `kernel/nsproxy.c` (`validate_nsset()`/`commit_nsset()`, used by
`setns(2)`) are compiled out entirely when the target lacks these options,
which would silently skip pid/user namespace installation during `setns(2)`
even though `vendor_kernel` fully implements both namespace types itself.

To make `vendor_kernel` install, refcount, and free its own `uts_namespace`/
`pid_namespace`/`user_namespace` objects correctly regardless of the target
kernel's `CONFIG_UTS_NS`/`CONFIG_PID_NS`/`CONFIG_USER_NS` setting, every
vendored call site uses local, unconditionally-compiled equivalents instead
of calling the real kernel functions or gating on these macros:

- `vns_get_uts_ns()` / `vns_put_uts_ns()` (`kernel/utsname.c`) replace
  `get_uts_ns()`/`put_uts_ns()`, including the two call sites in
  `kernel/nsproxy.c`.
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

`vendor_kernel` resolves a handful of the running kernel's namespace-adjacent
*function* symbols (e.g. `init_ipc_ns`, `init_cgroup_ns`) by name at module
load time via `shadow_hook_resolve()`; the namespace object slab caches
themselves (`uts_ns_cache`/`nsproxy_cachep`/`pid_ns_cachep`/`user_ns_cachep`)
are no longer resolved from the running kernel at all -- see "Slab-cache
consistency with the real kernel" above. For any namespace type whose
backing `CONFIG_*_NS` option is not built into the running kernel, the
corresponding resolved function does not exist, and `unshare(2)`/
`clone(2)`/`setns(2)` for that namespace type either falls back to
bookkeeping-only behaviour or fails with `-EINVAL` (this is the root cause
of `ns_ipc`/`ns_net` unshare test failures seen on kernels that ship with
`CONFIG_IPC_NS=n`/`CONFIG_NET_NS=n`, even though `vendor_kernel` itself
loads and activates successfully). `UTS_NS`, `PID_NS`, and `USER_NS` are the
exception: as documented above under "Namespace refcounting is fully
self-contained", vendor_kernel no longer depends on the target's
`CONFIG_UTS_NS`/`CONFIG_PID_NS`/`CONFIG_USER_NS` at all. The running kernel
still must be built with the remaining options for full namespace coverage:

- `CONFIG_NAMESPACES=y`
- `CONFIG_IPC_NS=y` (also requires `CONFIG_SYSVIPC=y` and/or `CONFIG_POSIX_MQUEUE=y`, since `IPC_NS depends on (SYSVIPC || POSIX_MQUEUE)`)
- `CONFIG_NET_NS=y`
- `CONFIG_CGROUPS=y`
- `CONFIG_TIME_NS=y`

`lkm4ctr/Kconfig`'s `LKM4CTR_VENDOR_KERNEL` option `select`s `CONFIG_UTS_NS`,
`CONFIG_PID_NS`, and `CONFIG_USER_NS` too (along with the rest), so any
future in-tree build sourcing that file still forces them on for
consistency and for any code elsewhere in the kernel that assumes them, but
`vendor_kernel` itself does not require any of the three any more for its
own uts/pid/user namespace support.

## Known remaining gaps

- POSIX message queues (`ipc/mqueue.c`) and SysV IPC (`ipc/msg.c`, `ipc/sem.c`, `ipc/shm.c`, `ipc/util.c`) are vendored sources but are not yet wired into `lkm4ctr/Makefile` or hooked to their syscalls; `mq_open()`/`msgget()`/etc. still resolve to `sys_ni_syscall()` on kernels built without `CONFIG_POSIX_MQUEUE`/`CONFIG_SYSVIPC`. Wiring these up needs dedicated syscall-hook glue (mirroring `vendor_kernel_syscalls.c`) plus Makefile changes. These files are unaffected by the `CONFIG_UTS_NS`/`CONFIG_PID_NS`/`CONFIG_USER_NS` self-containment above since they are not currently compiled into `lkm4ctr.ko`.
- `NET_NS` and `MNT_NS` are still resolved via optional function pointers (`vns_copy_net_ns_fn`/`vns_copy_mnt_ns_fn` in `glue/vendor_kernel_module.c`) rather than being fully vendored, so they still silently fall back to bookkeeping-only/no-op behaviour on a target kernel with `CONFIG_NET_NS=n`/`CONFIG_NAMESPACES` MNT support missing.
- `kernel/cgroup/namespace.c` and `kernel/time/namespace.c` store `user_ns` without taking a reference on it (`(void)user_ns;` in their copy functions), unlike `kernel/pid_namespace.c`/`kernel/user_namespace.c`/`kernel/utsname.c`/`ipc/namespace.c` which now use `vns_get_user_ns()`/`vns_put_user_ns()`; this is a pre-existing gap unrelated to `CONFIG_PID_NS`/`CONFIG_USER_NS` and was not changed here.
- `cred->user_ns` can be freed independently of nsproxy teardown via a deferred RCU callback (`put_cred_rcu()`), decoupled from task exit -- see "Slab-cache consistency with the real kernel" above for why this is an accepted, no-op-leak-only gap.
- overlayfs upper/work directory cloning failures are an in-tree overlayfs behavior on the stock vendor kernel binary; `vendor_kernel` (an out-of-tree LKM) cannot patch code that is already compiled into the running kernel, so this is out of scope for this module.

## Vendoring rules

- upstream sources were copied from `kernel-common` `android14-6.1` (kernel `6.1.124`)
- all non-static global symbols are renamed with a `vns_` prefix
- slab-cache users for `ipc_namespace`/`cgroup_namespace`/`time_namespace` are `kzalloc`/`kfree` (matches upstream, which also uses plain kzalloc/kmalloc for these); `uts_namespace`/`nsproxy`/`pid_namespace`/`user_namespace` allocate/free through vendor_kernel's own module-owned `kmem_cache_create()` caches (see "Slab-cache consistency with the real kernel" above), never the real kernel's private caches
- `get_uts_ns()`/`put_uts_ns()`/`get_pid_ns()`/`put_pid_ns()`/`get_user_ns()`/`put_user_ns()` call sites are replaced with `vns_get_uts_ns()`/`vns_put_uts_ns()`/`vns_get_pid_ns()`/`vns_put_pid_ns()`/`vns_get_user_ns()`/`vns_put_user_ns()` (see "Namespace refcounting is fully self-contained" above), and the `#ifdef CONFIG_PID_NS`/`#ifdef CONFIG_USER_NS` blocks gating pid/user namespace installation in `vns_sys_setns()`'s helpers are made unconditional, for the same reason
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
