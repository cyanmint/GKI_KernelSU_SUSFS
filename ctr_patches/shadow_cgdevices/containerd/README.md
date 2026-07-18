# Patching containerd/runc to use shadow_cgdevices

This directory contains a reference Go client (`shadow_cgdevices.go`) for the
shadow_cgdevices simulated cgroup device controller.  It is meant to be
**vendored into a patched containerd/runc build**.

> ⚠️ Scope: shadow_cgdevices is a *bookkeeping* layer (see `../README.md`).
> Rules are stored and can be queried, but actual device-access enforcement
> requires either the native cgroup device controller, a BPF `cgroup/dev`
> program, or an LSM.  Patch your runtime to record rules here instead of
> failing when the cgroup device controller is absent.

## Where containerd/runc configures device access

When starting a container, runc:
1. Creates a cgroup for the container.
2. Writes the OCI spec `linux.devices` entries and default rules to
   `devices.allow` / `devices.deny`.
3. Moves the container process into the cgroup.

On kernels without `CONFIG_CGROUP_DEVICE` step 2 fails.  The patched path
replaces step 2 with shadow_cgdevices calls:

| cgroup operation              | shadow_cgdevices equivalent                     |
|-------------------------------|-------------------------------------------------|
| Create a cgroup               | `session.Create(parentID)` → id                 |
| Write `devices.allow` rule    | `session.Allow(id, type, major, minor, access)` |
| Write `devices.deny` rule     | `session.Deny(id, type, major, minor, access)`  |
| `echo a > devices.deny` reset | `session.Reset(id)`                             |
| Query effective policy        | `session.Check(id, type, major, minor, access)` |
| Delete cgroup                 | `session.Destroy(id)`                           |

## Suggested integration points

1. **Open a session per container** and keep it for the container's lifetime:
   ```go
   s, err := shadowcgdevices.Open()
   defer s.Close()
   ```

2. **Create a shadow cgroup** corresponding to the container's cgroup:
   ```go
   cgID, err := s.Create(0) // 0 = root parent
   ```

3. **Apply the OCI spec device rules**:
   ```go
   // Allow all devices by default (matching the OCI default behaviour).
   s.Allow(cgID, shadowcgdevices.TypeAll, -1, -1,
           shadowcgdevices.AccessRead|shadowcgdevices.AccessWrite|shadowcgdevices.AccessMknod)

   // Then deny specific devices as the spec requires.
   s.Deny(cgID, shadowcgdevices.TypeChar, 5, 0,
          shadowcgdevices.AccessRead|shadowcgdevices.AccessWrite)
   ```

4. **Check before opening a device** (optional, for userspace enforcement):
   ```go
   ok, err := s.Check(cgID, shadowcgdevices.TypeChar, major, minor,
                      shadowcgdevices.AccessRead)
   ```

5. **Destroy on container teardown**:
   ```go
   s.Destroy(cgID)
   ```

## Example

```go
package main

import (
    "log"
    "github.com/…/shadowcgdevices"
)

func main() {
    s, err := shadowcgdevices.Open()
    if err != nil {
        log.Fatal(err)
    }
    defer s.Close()

    cgID, err := s.Create(0)
    if err != nil {
        log.Fatal(err)
    }

    // Deny all, then selectively allow /dev/null (1:3) and /dev/zero (1:5).
    s.Deny(cgID,  shadowcgdevices.TypeAll,   -1, -1, 0x07)
    s.Allow(cgID, shadowcgdevices.TypeChar,   1,  3, 0x07)
    s.Allow(cgID, shadowcgdevices.TypeChar,   1,  5, 0x07)

    ok, _ := s.Check(cgID, shadowcgdevices.TypeChar, 1, 3,
                     shadowcgdevices.AccessRead)
    log.Printf("/dev/null read allowed = %v", ok) // true

    ok, _ = s.Check(cgID, shadowcgdevices.TypeChar, 1, 8,
                    shadowcgdevices.AccessRead)
    log.Printf("/dev/random read allowed = %v", ok) // false

    s.Destroy(cgID)
}
```

## Building the client

```sh
cd ctr_patches/shadow_cgdevices/containerd
go mod init example.com/shadowcgdevices
go get golang.org/x/sys/unix
go build ./...
```

Keep `ABIVersion` in `shadow_cgdevices.go` in sync with
`SHADOW_CGDEV_ABI_VERSION` in the UAPI header.
