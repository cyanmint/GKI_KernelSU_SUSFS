# vendor_ns

`vendor_ns/` is a namespace subsystem source tree linked into the merged
`lkm4ctr.ko` for the GKI_KernelSU_SUSFS kernel. It is the always-on,
fully-vendored counterpart to [`shadow_ns`](../shadow_ns/README.md): every
symbol is `vendor_ns_`/`VENDOR_NS_`-prefixed so the two coexist in one `.ko`
without collision, even though only one is ever active at a time.

## Why this exists

`shadow_ns` is deliberately conservative: it computes, per running kernel,
which namespace types are *genuinely missing* (via `IS_ENABLED(CONFIG_*_NS)`
plus a load-time canary-symbol re-check) and takes over **only** those,
leaving every natively-supported type completely untouched so the real
kernel does the isolation. On a containerd-config kernel that has every type
builtin, `shadow_ns` is a pure passthrough that does nothing.

Some container-host use cases want the opposite: real, complete namespace
isolation driven entirely by the module, regardless of what the running
kernel natively supports — "vendor the whole namespace subsystem" rather
than "only fake what's missing". Reasons include running an unmodified
`containerd`/`runc`/`dockerd` on a kernel whose native namespace
implementation is patched out, ABI-mismatched, or deliberately disabled, or
wanting a single, self-contained, fully-introspectable namespace
implementation whose entire state is visible through diagfs.

`vendor_ns` is that subsystem. It hooks every namespace-related syscall
**unconditionally** (regardless of the running kernel's `CONFIG_*_NS`) and
maintains its own vendored, fully-managed namespace bookkeeping for every
namespace type. `VENDOR_NS_ALL_FLAGS` (pinned in `vendor_ns.h`) is the fixed
set of `CLONE_NEW*` bits it always tracks — unlike `shadow_ns`, whose tracked
set is whittled down to only the types the running kernel lacks. Each hooked
syscall runs the genuine kernel entry point first (so the host's real
namespace machinery still executes underneath) and then layers `vendor_ns`'s
vendored view on top: recording membership on `unshare`/`setns`/`clone`,
storing hostnames in the vendored UTS namespace, and translating pid/uid
results through the vendored pid/user namespaces. The entire vendored state is
visible through the `./mnt/vendor_ns/` diagfs tree.

## Conflict with `shadow_ns` (mutually exclusive)

`vendor_ns` and `shadow_ns` **cannot both be active at the same time.** Both
intercept the same syscall entry points (`unshare`/`setns`/`clone`/`clone3`/
`fork`/`vfork`, the UTS/PID/USER translation surface, and `/proc` content),
and the shared `shadow_hijack` hook engine permits only **one hook per
symbol**. If both installed their hooks simultaneously the second would fail
to install (or clobber the first), so this is enforced as a hard runtime
rule:

* Loading `vendor_ns` via its diagfs `control` file while `shadow_ns` is
  `active`/`loading` is refused with `-EBUSY` and a clear log message, and
  vice versa (see `lkm4ctr_diagfs_module_load()` in
  `../lkm4ctr_diagfs.c`).
* A global `load` (load-all, written to the root `./mnt/control`) loads
  `shadow_ns` (the default) and **silently skips** `vendor_ns`, rather than
  reporting a spurious failure. To switch, `unload` the active one first,
  then `load` the other.

Pick `shadow_ns` for "only fake what the kernel genuinely lacks, otherwise
get out of the way"; pick `vendor_ns` for "own the whole namespace subsystem
unconditionally".

> Note: `Kconfig` in this repository is a documentation/standalone artifact
> only — nothing sources it from a top-level `Kconfig`, so it is never
> actually evaluated in this out-of-tree build (see the header comment in
> `../Kconfig`). The mutual exclusion is therefore enforced purely at
> runtime by the diagfs load path, not by any Kconfig `depends on`/`conflicts`
> relationship. The Kconfig entries for `shadow_ns`/`vendor_ns` merely
> document the conflict.

## Per-namespace-type behaviour

Unlike `shadow_ns` (where each type is *either* left to the real kernel *or*
simulated, depending on the running config), `vendor_ns` vendors **every**
type unconditionally. As with `shadow_ns`, some types get real functional
isolation and others are reference-counted bookkeeping only — the difference
is that `vendor_ns` applies its column below *always*, never as a
config-gated fallback.

| Type | vendor_ns behaviour (always on) |
|------|----------------------------------|
| UTS (`CLONE_NEWUTS`) | **real**: per-namespace hostname/domainname. `sethostname`/`setdomainname` store into the vendored per-namespace `struct vns_new_utsname`, and `uname(2)` (hence libc `gethostname`/`getdomainname`) reads it back — the genuine syscall runs first, then vendor_ns overlays the vendored `nodename`/`domainname` fields into the returned `struct new_utsname`. |
| PID (`CLONE_NEWPID`) | **real numbering**: vendored `kernel/pid.c` `idr_alloc_cyclic()` + `RESERVED_PIDS` wraparound assigns namespace-local pids, and `getpid`/`getppid`/`gettid`/`getpgid`/`getsid` are translated through the vendored pid namespace's idr; `/proc` `getdents64` output is filtered so a task in a vendored child pid namespace only sees pids present in that namespace's idr. **Limitation:** a loadable module cannot rewrite `task_struct` parent links or reap tasks, so `zap_pid_ns_processes()`'s SIGKILL/`kernel_wait4()` cascade, signal-target translation for `kill`/`tgkill`/`rt_sigqueueinfo`, and `/proc/<pid>/{stat,status}` field rewriting are **not** performed (documented, same spirit as shadow_ns's NET note). |
| USER (`CLONE_NEWUSER`) | **real**: a genuine multi-extent `uid_map`/`gid_map` table (vendored `kernel/user_namespace.c` `map_id_up()`/`map_id_down()`); `getuid`/`geteuid`/`getgid`/`getegid` results are translated up through the calling namespace's vendored map. A fresh child user namespace starts with an empty map until one is written; the root vendored user namespace is the identity map. **Limitation:** `/proc/<pid>/{uid,gid}_map` write parsing and `/proc/<pid>/status` `Uid:`/`Gid:` rewriting are not performed from the module. |
| IPC (`CLONE_NEWIPC`) | bookkeeping only — reference-counted registry entry (vendored `ipc/namespace.c` shape). |
| MNT (`CLONE_NEWNS`) | bookkeeping only — reference-counted registry entry. Full mount-namespace vendoring from a loadable module is impractical (mount propagation lives in `fs/namespace.c` and the real `struct mnt_namespace`, neither ours to replace safely). |
| CGROUP (`CLONE_NEWCGROUP`) | bookkeeping only — reference-counted registry entry (vendored `kernel/cgroup/namespace.c` shape); no functional cgroup-namespace partitioning is safely addable from a module. |
| NET (`CLONE_NEWNET`) | bookkeeping only — real net-namespace isolation is inseparable from the whole networking stack (`net/core/net_namespace.c` touches routing, sockets, netfilter, sysctls) and cannot safely be vendored into a loadable module. **Same documented limitation `shadow_ns` states for NET.** |
| TIME (`CLONE_NEWTIME`) | bookkeeping only — time-namespace isolation needs per-namespace timekeeping offsets applied inside the vDSO/timekeeping core (`kernel/time/namespace.c`, the `timens_offsets` page), which a loadable module cannot install; `vendor_ns` tracks a reference-counted registry entry for parity with every other type. See the `CLONE_NEWTIME` caveat below. |

### `CLONE_NEWTIME` and legacy `clone(2)`

`CLONE_NEWTIME` has bit value `0x00000080`, which in the *legacy* `clone(2)`
ABI overlaps the low byte reserved for the `CSIGNAL` exit-signal field. The
real kernel therefore only accepts `CLONE_NEWTIME` via `unshare(2)` and
`clone3(2)`, never legacy `clone(2)`. `vendor_ns` includes `CLONE_NEWTIME`
in `VENDOR_NS_ALL_FLAGS`, but this is safe: no valid exit signal ever sets
bit `0x80` (the maximum signal number is 64 = `0x40`), so masking that bit
out of a legacy `clone()` flags word is a no-op for every real caller.
`vendor_ns.h` provides an `#ifndef CLONE_NEWTIME` fallback
(`0x00000080`) for older kernel uapi headers that predate the definition.

## Vendored kernel source (credits)

Per the module spec, `vendor_ns` reproduces the real kernel's own numbering
and lifecycle algorithms rather than inventing its own, so its replacement
bookkeeping behaves the way the genuine subsystem would. Each vendored
translation unit credits the kernel file it was adapted from in its own
header comment, in the form *"Vendored from kernel/… (Linux kernel,
GPL-2.0). Adapted for out-of-tree module use."* The only changes relative to
the original algorithms are (a) the `vendor_ns_`/`vns_` symbol prefix — so
this module's vendored copies coexist with the real builtin kernel functions
of the same name that it hooks unconditionally — and (b) the minimal
plumbing needed to expose the internal idr/tables/refcounts through this
submodule's diagfs files. No algorithm is "improved" or refactored.

| File | Vendored from |
|------|---------------|
| `fs/nsfs.c` | `fs/nsfs.c` + `fs/proc/generic.c` (`ns_common` inode-number allocation, biased above `PROC_DYNAMIC_FIRST`) and `include/linux/ns_common.h` (the `inum`+`refcount_t` namespace-object shape). |
| `kernel/utsname.c` | `kernel/utsname.c` (`clone_uts_ns()`'s `memcpy` of `struct new_utsname`) and `include/linux/utsname.h` (`struct uts_namespace`/`struct new_utsname`). |
| `kernel/user_namespace.c` | `kernel/user_namespace.c` (`struct uid_gid_map`'s extent-array `map_id_up()`/`map_id_down()` base-path lookup) and `include/linux/user_namespace.h` (`struct uid_gid_extent`/`struct uid_gid_map`/`struct user_namespace`). |
| `kernel/pid.c` + `kernel/pid_namespace.c` | `kernel/pid.c` (`alloc_pid()`'s `idr_alloc_cyclic()` cyclic pid allocation + `RESERVED_PIDS` wraparound) and `kernel/pid_namespace.c` (`create_pid_namespace()` idr init + `disable_pid_allocation()`). |
| `glue/vendor_ns_generic.c` | The common "payload + `ns_common`" shape of `ipc/namespace.c`, `fs/namespace.c`, `kernel/cgroup/namespace.c`, `net/core/net_namespace.c` and `kernel/time/namespace.c` (used for the bookkeeping-only IPC/MNT/CGROUP/NET/TIME types). |
| `kernel/nsproxy.c` | `kernel/nsproxy.c` (`create_new_namespaces()`'s per-type clone-or-reference logic and `free_nsproxy()`) and `include/linux/nsproxy.h` (`struct nsproxy` shape). |

The remaining glue files (`glue/vendor_ns_syscalls.c`, `glue/vendor_ns_procfs.c`,
`glue/vendor_ns_diag.c`, `glue/vendor_ns_module.c`) are vendor_ns's own plumbing — they use
the shared `shadow_hook` engine and the vendored cores above, and are not
adapted from any single kernel source file.

## Build

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr" modules
```

`vendor_ns` links directly with the `shadow_hijack` subsystem inside the same
`lkm4ctr.ko`; there is no separate `KBUILD_EXTRA_SYMBOLS` or load-ordering
step.

## Exported (module-internal) API

`vendor_ns` exposes no userspace ABI. The symbols the shared
`lkm4ctr_diagfs.c`/`lkm4ctr_main.c` wiring consumes are:

* `int vendor_ns_init(void)` / `void vendor_ns_exit(void)` — lifecycle
  callbacks driven by the diagfs `control` load/unload path; `init` builds the
  root vendored namespaces for every type and installs all hook groups
  unconditionally, `exit` removes them and tears down the registry.
* `size_t vendor_ns_diag_snprintf(char *buf, size_t buflen)` — aggregate
  status/reference rendering for `./mnt/vendor_ns/{status,references}`.
* `size_t vendor_ns_diag_snprintf_type(u32 type, char *buf, size_t buflen)` —
  per-namespace-type live-membership rendering for
  `./mnt/vendor_ns/<type>/namespaces`.

The `enum vendor_ns_type` values (`VENDOR_NS_TYPE_UTS`=0 … `VENDOR_NS_TYPE_TIME`=7,
`VENDOR_NS_TYPE_MAX`=8) live in `include/uapi/vendor_ns.h`.

## Diagfs paths

Once `lkm4ctr.ko` is mounted with `mount -t lkm4ctr diag <mnt>`, `vendor_ns`
is controlled through `/<mnt>/vendor_ns/control` and observed through
`/<mnt>/vendor_ns/status`, `/<mnt>/vendor_ns/hooks`,
`/<mnt>/vendor_ns/log`, `/<mnt>/vendor_ns/references`, and
`/<mnt>/vendor_ns/namespaces`. Each namespace type also has its own filtered
directory:

* `/<mnt>/vendor_ns/pid/...`
* `/<mnt>/vendor_ns/ipc/...`
* `/<mnt>/vendor_ns/mnt/...`
* `/<mnt>/vendor_ns/net/...`
* `/<mnt>/vendor_ns/user/...`
* `/<mnt>/vendor_ns/uts/...`
* `/<mnt>/vendor_ns/cgroup/...`
* `/<mnt>/vendor_ns/time/...`

Those per-type `control`/`status` files intentionally share the one real
`vendor_ns` lifecycle (there is a single `vendor_ns_init()`/`vendor_ns_exit()`
for the whole subsystem); their `namespaces` files are the type-filtered live
membership view. Remember `vendor_ns` will refuse to `load` while `shadow_ns`
(`/<mnt>/ns/`) is active, and vice versa.
