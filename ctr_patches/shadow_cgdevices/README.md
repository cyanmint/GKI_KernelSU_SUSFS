# shadow_cgdevices — simulated cgroup device controller

`shadow_cgdevices` is a **standalone loadable kernel module** that provides
bookkeeping for per-container device-access policies through ioctls on
`/dev/shadow_cgdevices`. A **patched containerd/runc** can configure virtual
device rules on a GKI kernel that was built **without** the cgroup v1 device
controller (`CONFIG_CGROUP_DEVICE`).

## Why this exists

The cgroup v1 device controller lets a runtime restrict container device access
by writing `type major:minor access` lines to `devices.allow` / `devices.deny`
in the cgroup hierarchy.  When the controller is absent those writes fail and
the runtime cannot enforce the OCI spec device restrictions.

`shadow_cgdevices` provides a parallel, ioctl-driven simulation.  A patched
runtime creates virtual "shadow cgroups", populates them with the same
allow/deny rules it would have written to cgroup files, and can query whether a
given device access is permitted according to those rules.

## Architecture

```
  patched containerd / runc               shadow_cgdevices.ko
  ┌─────────────────────────┐  ioctl  ┌──────────────────────────────────┐
  │ shadow_cgdevices.go     │────────▶│ /dev/shadow_cgdevices misc device│
  │  Open()                 │         │  per-fd "session"                 │
  │  Create(parentID)       │         │   owned[] -> shadow_cgroup        │
  │  Allow(id, 'c',5,1,rwm) │         │  global id -> shadow_cgroup       │
  │  Check(id, 'c',5,1,r)   │         │   (refcounted, xarray)            │
  │  Destroy(id)            │         └──────────────────────────────────┘
  └─────────────────────────┘
```

* **Session = open fd.**  All shadow cgroups owned by a session are freed on `close()`.
* **Ordered rule list per cgroup**: rules are kept in insertion order; `CHECK`
  returns the result of the *last matching rule* (same algorithm as cgroup v1).
  Default if no rule matches: **deny**.
* **Rule matching** supports wildcards: `major = -1` or `minor = -1` matches
  any major/minor; `dev_type = 'a'` matches both block and char devices.

## What is simulated

| Operation     | Behaviour                                                            |
|---------------|----------------------------------------------------------------------|
| `Create`      | Allocate a virtual cgroup with a stable integer id.                  |
| `Allow`       | Append an allow rule (type, major, minor, access).                   |
| `Deny`        | Append a deny rule.                                                  |
| `Reset`       | Remove all rules from a cgroup (restore deny-all default).           |
| `Check`       | Walk the rule list and return the last-matching allow/deny result.   |
| `Destroy`     | Release the session's ownership reference; cgroup freed when last.   |

### Honest scope / limitations

`shadow_cgdevices` is a **bookkeeping layer**, not real kernel enforcement:

* No actual device access is restricted.  A process inside the container can
  still open any device node that the filesystem permissions allow.
* Real per-container device restriction requires either the native cgroup
  device controller, a BPF `cgroup/dev` program, or a kernel LSM.
* Use this module so that a patched runtime can proceed without failing on
  device-configuration calls, and to record the intended policy for audit or
  userspace-enforced security layers.

## Files

| Path                                    | Purpose                                    |
|-----------------------------------------|--------------------------------------------|
| `include/uapi/shadow_cgdevices.h`       | Stable ioctl ABI shared with userspace.    |
| `shadow_cgdevices.c`                    | The kernel module.                         |
| `Makefile`                              | Out-of-tree build (`make KDIR=...`).       |
| `containerd/shadow_cgdevices.go`        | Reference Go client for containerd/runc.   |
| `containerd/README.md`                  | Runtime integration notes.                 |

## Building

```sh
cd ctr_patches/shadow_cgdevices
make KDIR=/path/to/kernel/build
sudo insmod shadow_cgdevices.ko
ls -l /dev/shadow_cgdevices
```

For an Android GKI cross build:
```sh
make -C /path/to/kernel/build M=$(pwd) ARCH=arm64 LLVM=1 modules
```

## Loading

```sh
insmod shadow_cgdevices.ko        # creates /dev/shadow_cgdevices (mode 0600)
dmesg | grep shadow_cgdevices     # "simulated cgroup device controller loaded (ABI v1)"
```

## ABI versioning

`SHADOW_CGDEV_IOC_ABI_VERSION` returns `SHADOW_CGDEV_ABI_VERSION`.  Clients
must verify it on open.  Bump in `include/uapi/shadow_cgdevices.h` whenever
the ioctl layout changes.
