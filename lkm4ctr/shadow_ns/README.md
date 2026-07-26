# shadow_ns

`shadow_ns/` is the namespace subsystem source linked into the merged
`lkm4ctr.ko` for the GKI_KernelSU_SUSFS kernel. It replaces the old
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
| PID (`CLONE_NEWPID`) | passthrough | **real**: vendored `kernel/pid.c`/`kernel/pid_namespace.c` algorithms driving per-namespace vpid↔rpid remapping plus shadow-tracked virtual process-group/session ids (`getpid`/`getppid`/`kill`/`tgkill`/`tkill`/`wait4`/`waitid`/`exit_group`/`setpgid`/`getpgid`/`getsid`/`setsid`/`ptrace`/`rt_sigqueueinfo`/`rt_tgsigqueueinfo`/`pidfd_open` hooked, plus fabricated `/proc/<pid>/ns/pid{,_for_children}` and rewritten `/proc/<pid>/{stat,status}` pid fields) |
| USER (`CLONE_NEWUSER`) | passthrough | **real**: a genuine multi-entry `uid_map`/`gid_map` table (`getuid`/`geteuid`/`getgid`/`getegid`/`getresuid`/`getresgid` hooked and translated through it, plus rewritten `/proc/<pid>/status` `Uid:`/`Gid:` lines and fabricated `/proc/<pid>/ns/user`), defaulting to the single-mapping docker/runc userns-remap shape (namespace id 0 == creator's real uid/gid) until a real map is installed |
| IPC (`CLONE_NEWIPC`) | passthrough | **real** for SysV IPC: each simulated IPC namespace has its own persistent synthetic id (`/proc/<pid>/ns/ipc` reports it), and `shadow_sysvipc`'s `msgget`/`semget`/`shmget`/`msgctl`/`semctl`/`shmctl`/`msgsnd`/`msgrcv`/`semop`/`semtimedop`/`shmat`/`shmdt` hooks route any task that is a member of one straight to a namespace-scoped shadow registry instead of the real, un-partitioned `init_ipc_ns` — see below and `../shadow_sysvipc/README.md`. POSIX message queues (`mq_*`, a separate `IPC_NS`-gating input alongside `SYSVIPC`) are not covered by this and remain bookkeeping-only. |
| NET (`CLONE_NEWNET`) | passthrough | bookkeeping only — real net namespace isolation is inseparable from the whole networking stack (`net/core/net_namespace.c` touches routing, sockets, netfilter, sysctls) and cannot safely be vendored into a loadable module |

### PID namespace isolation details

Vendors two real kernel algorithms from `kernel/pid.c` and
`kernel/pid_namespace.c` (see the citations in `shadow_ns_pid.c`) rather than
a plain incrementing counter, simplified to one level of nesting (no
nested-pid-namespace stacking): each simulated pid namespace keeps a
`struct idr` driven by `idr_alloc_cyclic()`, mirroring `alloc_pid()`'s own
cyclic-allocation-with-`RESERVED_PIDS`-wraparound policy, so the first task
registered into a fresh namespace deterministically becomes vpid 1 (that
namespace's *child reaper*, exactly like `copy_pid_ns()`). When that child
reaper's `exit_group(2)` fires (or, as a fallback, is next detected dead by
the periodic reaper if the syscall hook was bypassed), `shadow_ns_pidns_zap()`
vendors `zap_pid_ns_processes()`'s cascade: `disable_pid_allocation()` (no
further tasks may join) followed by an unblockable/unignorable `SIGKILL` to
every other task still resident in the namespace. (Real
`zap_pid_ns_processes()`'s orphan reparenting/reaping via
`forget_original_parent()`+`kernel_wait4()` needs `task_struct` parent/child
links this module does not own, so it is left to the host kernel's own,
already-working real parent chain.)

Real kernel semantics are preserved for `unshare(CLONE_NEWPID)`/`setns()`
into a pid namespace: the calling task itself is **not** moved — only its
`pending_pidns` (mirroring `nsproxy->pid_ns_for_children`) is set, so only
*future* children join the new namespace, exactly like the real kernel. A
direct `clone(2)`/`clone3(2)` with `CLONE_NEWPID` creates a fresh pidns for
that one child immediately.

Every syscall that consumes or produces a pid-namespace pid number is
translated: `getpid`/`getppid`/`kill`/`tgkill`/`tkill`/`wait4`/`waitid`
(`P_PID` and `P_PGID`) as before, plus `setpgid`/`getpgid`/`getsid`/`setsid`.
shadow_ns now keeps explicit per-task virtual process-group/session ids inside
each simulated pid namespace, so the namespace's own init correctly reports
`pid=pgid=sid=1` and later `setpgid(2)`/`setsid(2)` updates stay namespace-
local. `ptrace`, `rt_sigqueueinfo`/`rt_tgsigqueueinfo`, and `pidfd_open` are
translated too.

Known simplification: `tgkill`/`tkill` translate the pid argument through the
tgid-level map (thread ids are not tracked separately), and `waitid`'s
returned `siginfo.si_pid` is not translated back to a vpid (only the pid
*argument* is translated) — acceptable trade-offs for typical single/simple
threaded container-init use, but not a byte-for-byte replica of
multi-threaded nested pid namespace semantics.

**`/proc` isolation** (`shadow_ns_procfs.c`, alongside `/proc/*/setgroups`
below): a task with an active simulated PID namespace only ever sees its own
namespace's members under `/proc`, exactly like the real kernel's
`fs/proc/base.c` consulting `task_active_pid_ns()`:

- `open()`/`openat()`/`openat2()` of an (absolute, or relative to an
  already-open procfs directory fd) `/proc/<pid>[/...]` path is intercepted
  *before* the real syscall runs (not call-through-then-fallback, unlike the
  `setgroups` hook below — a real pid could otherwise coincidentally satisfy
  a "vpid" lookup and leak a host process outside the namespace). The
  leading numeric pid component is looked up in the same vpid↔rpid map
  `getpid()`/`kill()`/`wait4()` use: a registered member's vpid is
  translated to its real rpid and the open is served (via `filp_open()`/
  `file_open_root()`, since the translated, kernel-built path string can't
  be handed to the real syscall as a user pointer); a real pid that isn't a
  member of the namespace is rejected with `-ENOENT`, hiding it entirely.
- `getdents64()` on an fd for `/proc`'s own root directory is post-processed
  after the real syscall fills the caller's buffer: entries for real pids
  that aren't registered namespace members are dropped, and member entries
  are renamed from their real rpid to the namespace-local vpid, so `ls
  /proc`/`readdir(/proc)` only ever lists the namespace's own members.
- `read()`/`pread64()` on an already-open `/proc/<pid>/stat` or
  `/proc/<pid>/status` file are post-processed the same way: the real
  syscall always runs first, and only its result buffer is rewritten
  in-place (or, on any parse ambiguity or a rewritten buffer that would not
  fit the caller's original count, left completely untouched — the exact
  same fail-safe design as the `getdents64` filtering above). `stat`'s
  `pid` field (ahead of the `(comm)` that may itself embed spaces/parens)
  and its `ppid`/`pgrp`/`session` fields (proc(5) fields 4/5/6), and
  `status`'s `Pid:`/`PPid:` lines, are all rewritten to the namespace-local
  virtual values. `ppid` becomes `0` for the simulated child reaper just like
  a real pid namespace's init, while `pgrp`/`session` come from the same
  shadow-tracked virtual pgid/sid bookkeeping `getpgid()`/`getsid()` use,
  rather than from the host kernel's ambient process-group/session numbers.
  This is what makes an unmodified
  `ps`/`top` inside the namespace actually display namespace-local pids:
  every one of them trusts these two files' own embedded numbers over (or
  in addition to) the `getdents64`-renamed directory-listing name.
- `/proc/<pid>/ns/pid` and `/proc/<pid>/ns/pid_for_children` are fabricated
  (vendored from `fs/nsfs.c`/`fs/proc/namespaces.c`, which normally only
  wire these up when `CONFIG_PID_NS=y`): `getdents64()` on an already-open
  `.../ns` directory fd gets the two synthetic entries appended;
  `open()`/`openat()`/`openat2()` and `readlink()`/`readlinkat()` on either
  leaf are served once the real syscall has already failed with `-ENOENT`.
  `readlink()` returns `"pid:[<id>]"`/`"pid_for_children:[<id>]"` (the
  `shadow_ns` id standing in for a real `ns_common.inum`), and `open()`
  hands back an anon-inode fd tagged with that same id — recognized by
  `setns(2)`'s fallback path (`shadow_ns_core.c`) via
  `shadow_ns_procfs_nsfd_to_id()`, so an unmodified
  `open("/proc/<pid>/ns/pid")` + `setns(fd, CLONE_NEWPID)` sequence (exactly
  what nsenter/runc/dockerd already do) works with no userspace changes.

  The same `open()`/`readlink()` interception also prefers shadow-owned
  namespace objects for `/proc/<pid>/ns/ipc` (and the other fabricated
  entries) whenever the target task actually belongs to a simulated shadow
  namespace, even on kernels whose real procfs entry already exists. That
  keeps namespace-identity probes aligned with the shadow bookkeeping object
  instead of leaking the ambient host `ipc:[...]` id after a simulated
  `unshare(CLONE_NEWIPC)`.

Known limitations of this `/proc` isolation: only `/proc`'s own root listing
is filtered (subdirectory listings such as `/proc/<pid>/task/`, thread ids,
are left untouched); only `stat`/`status`'s pid-related fields are rewritten
(other procfs files that also embed real pids, e.g. `/proc/<pid>/wchan` or
per-thread `task/<tid>/stat`, are out of scope for this pass); and, like the
syscall-level translation above, this only ever activates when a simulated
PID namespace is actually active (i.e. `CONFIG_PID_NS` is genuinely absent
on the running kernel) — on a kernel with real `CONFIG_PID_NS`, the
kernel's own `fs/proc` already scopes `/proc` correctly and these hooks
no-op for that task.

### USER namespace isolation details

`getuid`/`geteuid`/`getgid`/`getegid`/`getresuid`/`getresgid` translate the
calling task's real uid/gid through a real, multi-entry `uid_map`/`gid_map`
table (`shadow_ns_idmap.c`), vendoring the *lookup shape* of
`kernel/user_namespace.c`'s `struct uid_gid_map` (a linear extent-array scan,
skipping only its rbtree fallback for very large maps, which
newuidmap/newgidmap's own line counts never approach in practice) rather
than the single hardcoded "namespace id 0 == creator's id" rule this module
used before. Until a real map is installed, translation falls back to that
same single implicit identity extent (namespace uid/gid 0 maps to the
creating task's real uid/gid), so the previous, still-correct
single-mapping docker/runc default keeps working unchanged.
`setuid`/`setgid` are deliberately left as passthrough (not virtualized).

**Known gap:** `shadow_ns_idmap.c` provides the map storage, validation
(`map_write()`-style extent-overlap/overflow checks) and translation
functions, and is ready to back real `newuidmap(1)`/`newgidmap(1)` writes,
but `/proc/<pid>/uid_map` and `gid_map` themselves are not yet intercepted
by `shadow_ns_procfs.c` the way `/proc/<pid>/ns/*` and `setgroups` are — a
write to those paths still goes to the real (unmodified) procfs file today.
Wiring that up is the same class of anon-inode-secure-fd read/write
fabrication `setgroups` below already does, and is a natural next step, but
was scoped out of this change given the size/risk of extending the
already-large `shadow_ns_procfs.c` interception surface without a full
regression pass against real `newuidmap`/`newgidmap` callers.

The same translated-id simulation now also applies to `/proc` *content*, not
just syscalls: `shadow_ns_hook_read()`/`shadow_ns_hook_pread64()`
(`shadow_ns_procfs.c`) rewrite an already-open `/proc/<pid>/status`'s
`Uid:`/`Gid:` lines to the mapped id for any target pid that is (or whose
real host tgid tracks as) a member of a simulated user namespace, via
`shadow_ns_userns_for_tgid()` (`shadow_ns_task.c`) — the same per-tgid
bookkeeping `shadow_ns_pidns_for_tgid()` already exposes for PID. Installed
alongside the PID `stat`/`status` rewriting above, in the same
`read`/`pread64` hooks, since both only ever need to run when either
`CLONE_NEWUSER` or `CLONE_NEWPID` is being simulated.

### `/proc/*/setgroups` (`shadow_ns_procfs.c`)

On a kernel that genuinely lacks `CONFIG_USER_NS`, `fs/proc/base.c` never
wires up the per-pid `uid_map`/`gid_map`/`setgroups` dentries at all (they
are `#ifdef CONFIG_USER_NS` entries in `tgid_base_stuff[]`/`tid_base_stuff[]`),
so `open()`/`openat2()` of `.../setgroups` fails with plain `-ENOENT`.
Recent runc/containerd unconditionally open their own `self/setgroups`
(often relative to a private, detached `fsopen("proc")`+`fsmount()`
descriptor rather than the real `/proc` mount) as a defensive procfs sanity
check before ever touching `uid_map`/`gid_map`, independent of whether the
container itself asked for a new user namespace — so the missing file
aborts *every* container start with "unsafe procfs detected ...
proc/self/setgroups: no such file or directory", not just ones that use
`CLONE_NEWUSER`.

Unlike `uid_map`/`gid_map`, `setgroups` needs no real per-namespace id-map
state to fake convincingly: real `setgroups(7)` semantics are just a
one-way "allow" → "deny" latch. `shadow_ns_procfs.c` hooks the raw
`open()`/`openat()`/`openat2()` syscalls the same way every other
`shadow_ns` hook does — call through to the real syscall first, and only
fabricate an anonymous file (implementing that same read/write latch) when
the real open genuinely failed with `-ENOENT` *and* the path unambiguously
names a `setgroups` leaf under a procfs-rooted pid directory
(`.../<pid|self|thread-self>/setgroups`, or a bare `"setgroups"` opened
relative to a dfd whose superblock is procfs — the shape a detached
`fsmount()` dirfd takes). Any other `-ENOENT` passes through untouched.

`shadow_ns_procfs_hooks` (the `open`/`openat`/`openat2`/`getdents64` hook
table shared by this and the `/proc` isolation feature above) installs
whenever *either* `CLONE_NEWUSER` or `CLONE_NEWPID` is being simulated (i.e.
`CONFIG_USER_NS` and/or `CONFIG_PID_NS` is genuinely absent); on a kernel
with both real natively, the setgroups file already exists and `/proc`
already scopes itself correctly, so these hooks are never installed at all.

The fabricated fd is created via `anon_inode_getfd_secure()` (a *private*,
per-fd inode) rather than the simpler `anon_inode_getfd()` (whose backing
inode is a single instance shared by every anon-inode fd on the system),
and that private inode's `->i_fop` is explicitly pointed at the module's
own `file_operations` (fetched back via a `fget()`/`fput()` pair around the
freshly-installed fd, since `anon_inode_getfd_secure()` only returns the
fd, not the `struct file`). This matters because modern runc/containerd
(via `securejoin`/`pathrs-lite`'s `ReopenFd()`) always "reopen" a
just-opened procfs fd a second time through the `/proc/thread-self/fd/<n>`
magic link, as a defensive check against symlink races — and that reopen
is a genuine VFS `open()` that goes through the inode's `->i_fop`, not the
already-open file's `->f_op`. `anon_inode_getfd()`'s shared singleton
inode never has its `->i_fop` touched, so it keeps `fs/inode.c`'s default
`no_open_fops` (`->open() == no_open()`, unconditionally `-ENXIO`) —
surfaced by runc/containerd as `unable to setup user: reopen
fsmount:fscontext:proc/self/setgroups: reopen fd N: no such device or
address`. The allow/deny latch state itself lives on that private inode's
`i_private` (not a separate heap allocation in `file->private_data`), so
both the original and the reopened `struct file` — which share the same
inode — observe the same state.

Note: the struct-file-returning sibling `anon_inode_getfile_secure()` is
deliberately *not* used here even though it would avoid the extra
`fget()`/`fput()` round trip — unlike `anon_inode_getfd()`/
`anon_inode_getfd_secure()`/`anon_inode_getfile()`, it has no
`EXPORT_SYMBOL(_GPL)` in `fs/anon_inodes.c`, so referencing it makes the
whole `lkm4ctr.ko` fail to `insmod` with "Unknown symbol
anon_inode_getfile_secure".

`anon_inode_getfd_secure()` itself *is* `EXPORT_SYMBOL_GPL()`'d, but a
direct call to it still made `lkm4ctr.ko` fail to `insmod` on both the
stock and the SukiSU/SUSFS-patched production GKI test kernels with
"Unknown symbol anon_inode_getfd_secure" — those kernels are built with
`CONFIG_TRIM_UNUSED_KSYMS`, which strips the export for any symbol unused
by built-in code even though the function itself stays compiled in and
visible in `/proc/kallsyms` (the same class of failure previously hit with
`path_put()` in `shadow_mqueue`). `shadow_ns_procfs.c` therefore resolves
`anon_inode_getfd_secure` via `shadow_hook_resolve()` (kprobe-based
kallsyms lookup) instead of calling it directly, exactly like
`shadow_mqueue` does for `kern_path`/`vfs_mkdir`/`path_mount`/`path_put`.

## Build

```sh
make -C /path/to/kernel/build M="$PWD/lkm4ctr/lkm4ctr" modules
```

`shadow_ns` now links directly with the `shadow_hijack` subsystem inside the
same `lkm4ctr.ko`; there is no separate `KBUILD_EXTRA_SYMBOLS` or load
ordering step anymore.

## Exported API

* `bool shadow_ns_type_simulated(u32 type)` — true if this build fakes/
  bookkeeps `type` (i.e. the kernel build genuinely lacks native support).
* `bool shadow_ns_type_real(u32 type)` — true if the simulation for `type`
  performs genuine functional isolation rather than bookkeeping only
  (UTS, PID, USER, and now IPC for the SysV IPC data path via
  `shadow_sysvipc`); false for NET/CGROUP/MNT (bookkeeping-only when
  simulated).

Not currently consumed by any other in-tree module — `lkm4ctr_checker`
deliberately has zero build/load-time dependencies and instead recomputes the
same `IS_ENABLED(CONFIG_*_NS)` logic independently at compile time (both
modules are built against the identical target kernel config in the same DDK
image, so the results are guaranteed to agree). Exported for any external
consumer that does want to query at runtime whether a namespace type is being
simulated and, if so, whether that simulation is functionally real.

## Diagfs paths

Once `lkm4ctr.ko` is mounted with `mount -t lkm4ctr diag <mnt>`, shadow_ns is
controlled through `/<mnt>/ns/control` and observed through `/<mnt>/ns/status`,
`/<mnt>/ns/hooks`, `/<mnt>/ns/log`, and `/<mnt>/ns/namespaces`. Each namespace
type also has its own filtered directory:

* `/<mnt>/ns/pid/...`
* `/<mnt>/ns/ipc/...`
* `/<mnt>/ns/mnt/...`
* `/<mnt>/ns/net/...`
* `/<mnt>/ns/user/...`
* `/<mnt>/ns/uts/...`
* `/<mnt>/ns/cgroup/...`

Those per-type `control`/`status` files intentionally share the one real
shadow_ns lifecycle; their `namespaces` files are the type-filtered live
membership view.
