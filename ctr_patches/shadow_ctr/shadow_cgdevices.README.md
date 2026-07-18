# shadow_cgdevices — simulated cgroup device controller

`shadow_cgdevices` is one of the subsystems linked into the combined
`shadow_ctr.ko` module (see `README.md` in this directory for the umbrella
overview). It keeps a shadow copy of cgroup-v1-style device allow/deny rules
and can now enforce that policy transparently at real device-open points.

It is still **not** a full replacement for the native `devices` cgroup v1
controller. The practical goal is narrower: keep rule authorship in a small,
self-contained ioctl API, then optionally bind a real cgroup to that shadow
policy so later `/dev/*` opens are checked in-kernel without patching the
container runtime binary itself.

## Why this exists

When `CONFIG_CGROUP_DEVICE=n`, two distinct things are true:

1. There is no native device-controller rule store (`devices.allow`,
   `devices.deny`, `devices.list`).
2. There is no native enforcement hook such as `devcgroup_check_permission()` to
   intercept, because that code was never compiled in.

Recent `runc` / `containerd` / `dockerd` are usually fine with that. They probe
which cgroup controllers actually exist and generally **skip** configuring a
missing `devices` controller instead of hard-failing container startup.

So this module's value-add is **optional real enforcement**, not "make modern
runc start at all". Startup is often already fine; what is missing is a kernel
policy check for device opens after the container is running.

## Architecture

```text
OCI hook / init helper                     shadow_ctr.ko
┌───────────────────────────────┐   ioctl  ┌─────────────────────────────────┐
│ open /dev/shadow_cgdevices    │────────▶│ misc device session              │
│ CREATE shadow cgroup          │         │  owned[] -> shadow_cgroup        │
│ RULE_ADD allow/deny entries   │         │  id -> shadow_cgroup (xarray)    │
│ BIND current real cgroup      │         │  real cgroup id -> shadow policy │
└───────────────────────────────┘         └─────────────────────────────────┘
                                                      │
                                                      │ ftrace hook
                                                      ▼
                                         chrdev_open() / blkdev_open()*
                                                      │
                                                      ▼
                                     cgdev_check_access(last-match wins)
```

`*` block-device coverage is best-effort and symbol-name/version dependent;
character-device coverage through `chrdev_open()` is the stable path and the one
that matters most for common OCI allowlists (`/dev/null`, `/dev/zero`,
`/dev/random`, `/dev/tty`, ...).

### Rule model

* **Session = open fd.** All shadow cgroups owned by a session are released on
  `close()`.
* **Ordered rule list per cgroup.** The *last* matching rule wins, matching the
  cgroup v1 controller semantics.
* **Default if no rule matches: deny.**
* **Wildcards:** `major = -1`, `minor = -1`, or `dev_type = 'a'`.
* **Transparent enforcement after bind:** once a real cgroup id is bound to a
  shadow cgroup, later opens by tasks in that cgroup are checked automatically;
  no per-open ioctl is needed.
* **Default if no bind exists: permissive.** Unbound cgroups behave like today's
  kernel-without-device-controller situation: this module does not block them.

## What real enforcement now happens

The module installs ftrace hooks with the shared `shadow_hook` helper and checks
shadow rules before calling the real kernel open handler:

| Kernel entry point | Status | Notes |
|--------------------|--------|-------|
| `chrdev_open`      | Enforced | Primary stable hook for character devices. |
| `blkdev_open`      | Best effort | Hook installed only if that symbol exists with the expected prototype. |

Access bits are derived from the open request (`FMODE_READ` / `FMODE_WRITE`).
`mknod` is still represented in the rule language for parity with cgroup v1, but
it is **not** evaluated by these open hooks because an `open()` is not a device
creation operation.

## What still needs an explicit userspace step

Rule authorship and cgroup association are still explicit operations:

1. `CREATE` a shadow cgroup.
2. `RULE_ADD` the desired allow/deny list.
3. `BIND` the caller's **current real cgroup** to that shadow cgroup.

That means you still need a tiny helper (for example `cgdevctl`) invoked from an
OCI hook, post-create script, or container init path. The important change is
that this helper only performs **setup once per container/cgroup**. It does **not**
need to stay in the hot path for every later `open()` call, and it does **not**
require patching `containerd` or `runc` themselves.

A practical deployment model is:

```text
container starts -> OCI hook joins target cgroup -> cgdevctl bind-rules -> done
```

After that, ordinary processes in the same cgroup hit transparent in-kernel
checks automatically.

## Honest scope / limitations

This module is intentionally **not** attempting to fake the full cgroupfs
`devices.*` file tree.

Why not?

* With `CONFIG_CGROUP_DEVICE=n`, the real devices controller subsystem is absent,
  so faking `devices.allow` / `devices.deny` would mean invasive cgroup/kernfs/VFS
  surgery well beyond what is safe to maintain as a small out-of-tree LKM.
* Modern runtimes already tolerate the controller being absent, so that risky
  work is usually unnecessary for container startup.
* The tractable, high-value part is real device-open enforcement keyed by the
  task's actual cgroup membership, and that is what this module now does.

Other limitations that remain:

* No automatic discovery of which shadow policy should apply to which container;
  a userspace helper still has to perform the one-time `BIND`.
* Block-device enforcement is version-dependent because the block open path has
  moved around more than the character-device path.
* The rule store remains an in-memory shadow policy owned by this module; it is
  not visible as native cgroupfs controller files.
* Unbound cgroups are intentionally left permissive.

## Files

| Path                              | Purpose |
|-----------------------------------|---------|
| `include/uapi/shadow_cgdevices.h` | Stable ioctl ABI shared with userspace. |
| `shadow_cgdevices.c`              | Kernel module: rule store, bind table, open hooks. |
| `Makefile`                        | Out-of-tree build (`make KDIR=...`). |

## Building

```sh
cd ctr_patches/shadow_ctr
make KDIR=/path/to/kernel/build
sudo insmod shadow_ctr.ko
ls -l /dev/shadow_cgdevices
```

For an Android GKI cross build:

```sh
make -C /path/to/kernel/build M=$(pwd) ARCH=arm64 LLVM=1 modules
```

## Loading

```sh
insmod shadow_ctr.ko
 dmesg | grep shadow_cgdevices
```

Expect a log line showing the misc device path and how many transparent hooks
were installed on this kernel.

## ABI versioning

`SHADOW_CGDEV_IOC_ABI_VERSION` returns `SHADOW_CGDEV_ABI_VERSION`. Existing
commands remain compatible; new helpers that want transparent enforcement should
also use `SHADOW_CGDEV_IOC_BIND`.
