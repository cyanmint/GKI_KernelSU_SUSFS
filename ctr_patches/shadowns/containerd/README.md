# Patching containerd/runc to use shadowns

This directory contains a reference Go client (`shadowns.go`) for the shadowns
simulated namespace subsystem. It is meant to be **vendored into a patched
containerd/runc build**. The client speaks the ioctl ABI defined in
`../include/uapi/shadowns.h`.

> ⚠️ Scope: shadowns is a *simulation/bookkeeping* layer (see `../README.md`).
> UTS (hostname/domainname) is genuinely isolated; the other namespace types are
> reference-counted identities without real kernel-level isolation. Patch your
> runtime accordingly and do not assume full container isolation from this alone.

## Where namespaces are set up

In the OCI flow, the low-level runtime (`runc`) is what actually creates Linux
namespaces, based on the `linux.namespaces` array in the OCI runtime spec that
containerd hands it. The native path is:

* `runc` calls `clone(2)`/`unshare(2)` with `CLONE_NEW*` flags, and
* joins existing namespaces with `setns(2)` (via the `nsenter` C shim).

On a kernel without `CONFIG_*_NS`, those calls fail with `EINVAL`/`ENOSYS`. The
shadow path replaces/augments them with `shadowns` ioctls.

## Suggested integration points

1. **Open a session per container.** When the runtime begins creating a
   container, call `shadowns.Open()` and keep the `*Session` for the lifetime of
   the bring-up. Closing it releases every shadow namespace the container held.

2. **Map spec namespaces to shadow verbs.** For each entry in
   `spec.Linux.Namespaces`:
   * no `Path` → `session.Unshare(mapType(ns.Type))` (create + join a new one),
   * with `Path` → resolve the path to a stored shadow id and
     `session.SetNS(id, mapType(ns.Type))` (join existing).

   ```go
   func mapType(t specs.LinuxNamespaceType) shadowns.Type {
       switch t {
       case specs.UTSNamespace:     return shadowns.TypeUTS
       case specs.IPCNamespace:     return shadowns.TypeIPC
       case specs.MountNamespace:   return shadowns.TypeMNT
       case specs.PIDNamespace:     return shadowns.TypePID
       case specs.NetworkNamespace: return shadowns.TypeNET
       case specs.UserNamespace:    return shadowns.TypeUSER
       case specs.CgroupNamespace:  return shadowns.TypeCGROUP
       }
       return shadowns.TypeAny
   }
   ```

3. **Apply the hostname.** After joining a UTS shadow namespace, apply the spec
   hostname to it instead of calling `sethostname(2)`:

   ```go
   if spec.Hostname != "" {
       _, _, _ = session.Unshare(shadowns.TypeUTS) // if not already unshared
       _ = session.SetHostname(spec.Hostname, spec.Domainname)
   }
   ```

4. **Persist shadow ids for join-by-path.** containerd tracks namespace paths
   (e.g. CNI netns) so other containers can join them. Store the returned shadow
   id alongside the container/sandbox metadata and translate `Path`→id when a
   later container references it.

## Example

```go
package main

import (
	"log"

	"github.com/…/shadowns" // this package, vendored
)

func main() {
	s, err := shadowns.Open()
	if err != nil {
		log.Fatal(err)
	}
	defer s.Close()

	// New UTS + NET + IPC namespaces for a fresh container.
	if _, _, err := s.Unshare(shadowns.TypeUTS); err != nil {
		log.Fatal(err)
	}
	if _, _, err := s.Unshare(shadowns.TypeNET); err != nil {
		log.Fatal(err)
	}
	if _, _, err := s.Unshare(shadowns.TypeIPC); err != nil {
		log.Fatal(err)
	}

	if err := s.SetHostname("container01", ""); err != nil {
		log.Fatal(err)
	}
	host, _, _ := s.Hostname()
	log.Printf("shadow UTS hostname = %q", host)
}
```

## Building the client

The client depends only on `golang.org/x/sys/unix`:

```sh
cd ctr_patches/shadowns/containerd
go mod init example.com/shadowns   # or vendor into an existing module
go get golang.org/x/sys/unix
go build ./...
```

Keep `ABIVersion` in `shadowns.go` in sync with `SHADOWNS_ABI_VERSION` in the
UAPI header; `Open()` refuses to run against a module with a different ABI.
