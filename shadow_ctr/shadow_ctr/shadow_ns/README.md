# shadow_ns

`shadow_ns/` is the namespace subsystem source linked into the merged
`shadow_ctr.ko` for the GKI_KernelSU_SUSFS kernel. It replaces the old
`shadow_ns_base` +
`shadow_ns_{uts,ipc,mnt,pid,net,user,cgroup}` family.

## Why this exists

dockerd/containerd relies on `unshare(2)`/`clone(2)` with `CLONE_NEW*` flags
to isolate a container's init process from the host. The previous
`shadow_ns_base.c` unconditionally stripped every `CLONE_NEW*` flag before
calling the real syscall and substituted its own reference-counted bookkeeping
object instead — so even when the kernel had genuine, compiled-in namespace
support, dockerd's `unshare()` never actually isolated anything: the
container's "namespace" was bookkeeping only, and the process kept running in
the host's real namespaces.

## Root cause and design

`.github/workflows/scripts/kernel_builder.py`'s `CONTAINERD_CONFIG` (applied
whenever `use_containerd` is true, which is the default in
`config.py`) forces `CONFIG_UTS_NS=y`, `CONFIG_PID_NS=y`, `CONFIG_IPC_NS=y`,
`CONFIG_USER_NS=y`, `CONFIG_NET_NS=y` — the production kernel already has
real, native namespace support for every type. Mount namespaces
(`CLONE_NEWNS`, `fs/namespace.c`) have no dedicated per-type Kconfig gate at
all and are always compiled in; cgroup namespaces only need
`CONFIG_CGROUPS=y` (always set).

`shadow_ns` computes, per namespace type, whether this specific kernel
build has genuine support via `IS_ENABLED(CONFIG_UTS_NS)` /
`IS_ENABLED(CONFIG_IPC_NS)` / `IS_ENABLED(CONFIG_USER_NS)` /
`IS_ENABLED(CONFIG_PID_NS)` / `IS_ENABLED(CONFIG_NET_NS)` /
`IS_ENABLED(CONFIG_CGROUPS)`. `CLONE_NEWNS` (MNT) has no `CONFIG_MNT_NS`
symbol to check the way every other type does, so it instead keys off
`IS_ENABLED(CONFIG_NAMESPACES)` itself (the parent menuconfig, `default
!EXPERT`, i.e. effectively always on):

* **Builtin type** → the corresponding `CLONE_NEW*` bit is left untouched in
  `unshare`/`clone`/`clone3`, so the real kernel namespace code runs and
  provides genuine isolation. shadow_ns does not get involved at all.
* **Genuinely absent type** → shadow_ns takes over that type's namespace
  handling completely, including on a kernel built with
  `CONFIG_NAMESPACES=n` entirely: `init/Kconfig` nests
  `UTS_NS`/`IPC_NS`/`USER_NS`/`PID_NS`/`NET_NS` inside
  `menuconfig NAMESPACES ... if NAMESPACES ... endif`, so turning the parent
  off forces all five children off too — `SHADOW_NS_BUILTIN_FLAGS` collapses
  to "none of these five builtin" automatically, no special-casing needed.
  MNT is also gated on that same `CONFIG_NAMESPACES` symbol (see below), so
  it drops out alongside the other five and falls back to bookkeeping too.
  `unshare(2)`/`setns(2)`/`clone(2)`/`clone3(2)` have no `CONFIG_NAMESPACES`
  guard either (always compiled in `kernel/fork.c`), so the syscalls are
  always available for this module to hook.

This is a superset of the old "stub" behaviour: where the old modules always
faked every type, the new module only fakes what's genuinely missing, and for
what it does fake it now performs **real functional isolation**, not just
inert bookkeeping:

| Type | If builtin | If absent (simulated) |
|------|-----------|------------------------|
| MNT (`CLONE_NEWNS`) | if `IS_ENABLED(CONFIG_NAMESPACES)` (effectively always, `default !EXPERT`; no dedicated `CONFIG_MNT_NS` symbol exists so the parent menuconfig is used as a defensive proxy) | bookkeeping only — same generic id/refcount registry as IPC/NET/CGROUP; the real, always-compiled-in mount-namespace code in `fs/namespace.c` keeps running regardless, this only adds a parallel bookkeeping entry |
| CGROUP (`CLONE_NEWCGROUP`) | if `CONFIG_CGROUPS=y` (every GKI defconfig sets this) | bookkeeping only — same generic id/refcount registry as IPC/NET below; no functional cgroup-namespace partitioning to add safely from a module |
| UTS (`CLONE_NEWUTS`) | passthrough | **real**: per-namespace hostname/domainname (`sethostname`/`setdomainname`/`uname` hooked) |
| PID (`CLONE_NEWPID`) | passthrough | **real**: per-namespace vpid↔rpid remapping (`getpid`/`getppid`/`kill`/`tgkill`/`tkill`/`wait4`/`waitid` hooked) |
| USER (`CLONE_NEWUSER`) | passthrough | **real**: creator's uid/gid appear as 0 inside the namespace (`getuid`/`geteuid`/`getgid`/`getegid`/`getresuid`/`getresgid` hooked), matching the common docker/runc single-mapping userns-remap shape |
| IPC (`CLONE_NEWIPC`) | passthrough | bookkeeping only — falls back to the single global `init_ipc_ns`; there is nothing extra to isolate |
| NET (`CLONE_NEWNET`) | passthrough | bookkeeping only — real net namespace isolation is inseparable from the whole networking stack (`net/core/net_namespace.c` touches routing, sockets, netfilter, sysctls) and cannot safely be vendored into a loadable module |

### PID namespace isolation details

Modelled on `kernel/pid_namespace.c`'s single-level id mapping (`pid_nr_ns()`
translating a task's namespace-local number via its `numbers[]` array),
simplified to one level (no nested-pid-namespace stacking): each simulated
pid namespace keeps a pair of xarrays mapping real tgid ↔ namespace-local
vpid, assigned in join order starting at 1. Real kernel semantics are
preserved for `unshare(CLONE_NEWPID)`/`setns()` into a pid namespace: the
calling task itself is **not** moved — only its `pending_pidns` (mirroring
`nsproxy->pid_ns_for_children`) is set, so only *future* children join the
new namespace, exactly like the real kernel. A direct `clone(2)`/`clone3(2)`
with `CLONE_NEWPID` creates a fresh pidns for that one child immediately.

Known simplification: `tgkill`/`tkill` translate the pid argument through the
tgid-level map (thread ids are not tracked separately), and `waitid`'s
returned `siginfo.si_pid` is not translated back to a vpid (only the pid
*argument* is translated) — acceptable trade-offs for typical single/simple
threaded container-init use, but not a byte-for-byte replica of
multi-threaded nested pid namespace semantics.

### USER namespace isolation details

Rather than implementing arbitrary `uid_map`/`gid_map` parsing (which would
require hooking `write(2)` on `/proc/<pid>/{u,g}id_map` — too invasive/unsafe
for an out-of-tree module), the creating task's real uid/gid is captured at
`CLONE_NEWUSER` time, and `getuid`/`geteuid`/`getgid`/`getegid`/
`getresuid`/`getresgid` report 0 for any task inside that simulated user
namespace — matching the default single-mapping shape used by
`docker run --userns-remap`/runc when nothing more specific is configured.
`setuid`/`setgid` are deliberately left as passthrough (not virtualized).

## Build

```sh
make -C /path/to/kernel/build M="$PWD/shadow_ctr/shadow_ctr" modules
```

`shadow_ns` now links directly with the `shadow_hijack` subsystem inside the
same `shadow_ctr.ko`; there is no separate `KBUILD_EXTRA_SYMBOLS` or load
ordering step anymore.

## Exported API

* `bool shadow_ns_type_simulated(u32 type)` — true if this build fakes/
  bookkeeps `type` (i.e. the kernel build genuinely lacks native support).
* `bool shadow_ns_type_real(u32 type)` — true if the simulation for `type`
  performs genuine functional isolation rather than bookkeeping only
  (UTS, PID, USER); false for IPC/NET/CGROUP/MNT (bookkeeping-only when
  simulated).

Not currently consumed by any other in-tree module — `shadow_ctr_checker`
deliberately has zero build/load-time dependencies and instead recomputes the
same `IS_ENABLED(CONFIG_*_NS)` logic independently at compile time (both
modules are built against the identical target kernel config in the same DDK
image, so the results are guaranteed to agree). Exported for any external
consumer that does want to query at runtime whether a namespace type is being
simulated and, if so, whether that simulation is functionally real.
