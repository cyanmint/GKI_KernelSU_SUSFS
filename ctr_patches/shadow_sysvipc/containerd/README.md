# Patching containerd/runc to use shadow_sysvipc

This directory contains a reference Go client (`shadow_sysvipc.go`) for the
shadow_sysvipc simulated System V IPC subsystem.  It is meant to be **vendored
into a patched containerd/runc build**.

> ⚠️ Scope: shadow_sysvipc is a *bookkeeping* layer (see `../README.md`).
> Resource creation/lookup semantics mirror SysV IPC, but actual message
> passing, semaphore operations, and shared-memory mapping are **not**
> provided.  Patch your runtime to skip or stub those operations and use this
> module only for resource-identity management.

## Where SysV IPC resources are managed by a runtime

runc processes the OCI spec's `linux.namespaces` array and creates an IPC
namespace via `clone(CLONE_NEWIPC)`.  On kernels without `CONFIG_IPC_NS` that
call fails.  The `shadowns` module already handles the *namespace* side.
`shadow_sysvipc` handles the *resource objects* that would live inside that
namespace (message queues, semaphore sets, shared-memory segments).

## Suggested integration points

1. **Open a session per container** when the runtime begins creating it:
   ```go
   s, err := shadowsysvipc.Open()
   // keep s for the container's lifetime; s.Close() frees everything
   ```

2. **Map msgget/semget/shmget calls** to `s.Create(...)`:
   ```go
   id, err := s.Create(shadowsysvipc.TypeMsgQ, shadowsysvipc.IPC_PRIVATE,
                       shadowsysvipc.IPC_CREAT|0600, 0, 0)
   ```

3. **Query resource metadata** with `s.Stat(type, id)`.

4. **Release resources** explicitly with `s.Destroy(type, id)`, or rely on
   session close to free everything automatically.

## Example

```go
package main

import (
    "log"
    "github.com/…/shadowsysvipc"
)

func main() {
    s, err := shadowsysvipc.Open()
    if err != nil {
        log.Fatal(err)
    }
    defer s.Close()

    // Private message queue for a container.
    id, err := s.Create(shadowsysvipc.TypeMsgQ, shadowsysvipc.IPC_PRIVATE,
                        shadowsysvipc.IPC_CREAT|0600, 0, 0)
    if err != nil {
        log.Fatal(err)
    }
    log.Printf("shadow msgq id = %d", id)

    // Shared semaphore set with a well-known key.
    semID, err := s.Create(shadowsysvipc.TypeSem, 0x1234,
                           shadowsysvipc.IPC_CREAT|0644, 4, 0)
    if err != nil {
        log.Fatal(err)
    }
    key, _, nsems, _, err := s.Stat(shadowsysvipc.TypeSem, semID)
    log.Printf("shadow sem id=%d key=%d nsems=%d", semID, key, nsems)
}
```

## Building the client

```sh
cd ctr_patches/shadow_sysvipc/containerd
go mod init example.com/shadowsysvipc
go get golang.org/x/sys/unix
go build ./...
```

Keep `ABIVersion` in `shadow_sysvipc.go` in sync with
`SHADOW_SYSVIPC_ABI_VERSION` in the UAPI header.
