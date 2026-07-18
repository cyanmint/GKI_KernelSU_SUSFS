# Patching containerd/runc to use shadow_mqueue

This directory contains a reference Go client (`shadow_mqueue.go`) for the
shadow_mqueue simulated POSIX message queue subsystem.  It is meant to be
**vendored into a patched containerd/runc build**.

> ✅ shadow_mqueue is *functionally complete*: Send/Receive transfer real bytes
> through the kernel with priority ordering and blocking semantics.  Unlike the
> namespace or SysV IPC simulations, message passing here actually works.

## Where runc uses POSIX message queues

`runc init` (the child side of a container start) communicates with the parent
`runc` process through a POSIX message queue opened at the start of `runc run`.
The typical flow:

1. Parent opens `/runc-<id>` with `O_CREAT | O_RDWR`.
2. Parent forks; child inherits the queue name.
3. Child sends synchronisation messages to unblock the parent at each stage of
   initialisation (namespace setup, pivot_root, exec).
4. Parent receives and acknowledges each stage.
5. Both sides close and unlink the queue once the container is running.

Replace each step with the corresponding `shadow_mqueue` call:

| POSIX call          | shadow_mqueue equivalent                                  |
|---------------------|-----------------------------------------------------------|
| `mq_open(name, …)`  | `session.MQOpen(name, oflag, mode, &attr)` → handle       |
| `mq_send(mqd, …)`   | `session.Send(handle, prio, timeoutNs, msg)`              |
| `mq_receive(mqd, …)`| `session.Receive(handle, timeoutNs)` → msg, prio          |
| `mq_close(mqd)`     | `session.MQClose(handle)`                                 |
| `mq_unlink(name)`   | `session.MQUnlink(name)`                                  |
| `mq_getattr(…)`     | `session.GetAttr(handle)`                                 |
| `mq_setattr(…)`     | `session.SetAttr(handle, newFlags)`                       |

## Suggested integration points

1. **Open a session per process** (parent and child each open their own
   `/dev/shadow_mqueue` fd):
   ```go
   s, err := shadowmqueue.Open()
   defer s.Close()
   ```

2. **Parent creates the queue before forking**:
   ```go
   handle, attr, err := s.MQOpen("/runc-"+id,
       shadowmqueue.O_CREAT|shadowmqueue.O_RDWR,
       0600, &shadowmqueue.Attr{MaxMsg: 8, MsgSize: 256})
   ```

3. **Child sends each sync byte**:
   ```go
   err = s.Send(handle, 0, shadowmqueue.BlockForever, []byte{syncByte})
   ```

4. **Parent receives each sync byte**:
   ```go
   msg, _, err := s.Receive(handle, shadowmqueue.BlockForever)
   ```

5. **Both sides close and the parent unlinks**:
   ```go
   s.MQClose(handle)
   s.MQUnlink("/runc-" + id)
   ```

## Example

```go
package main

import (
    "log"
    "github.com/…/shadowmqueue"
)

func main() {
    s, err := shadowmqueue.Open()
    if err != nil {
        log.Fatal(err)
    }
    defer s.Close()

    handle, _, err := s.MQOpen("/test-queue",
        shadowmqueue.O_CREAT|shadowmqueue.O_RDWR, 0600, nil)
    if err != nil {
        log.Fatal(err)
    }

    if err := s.Send(handle, 10, shadowmqueue.BlockForever, []byte("hello")); err != nil {
        log.Fatal(err)
    }

    msg, prio, err := s.Receive(handle, shadowmqueue.BlockForever)
    if err != nil {
        log.Fatal(err)
    }
    log.Printf("received %q prio=%d", msg, prio)

    s.MQClose(handle)
    s.MQUnlink("/test-queue")
}
```

## Building the client

```sh
cd ctr_patches/shadow_mqueue/containerd
go mod init example.com/shadowmqueue
go get golang.org/x/sys/unix
go build ./...
```

Keep `ABIVersion` in `shadow_mqueue.go` in sync with
`SHADOW_MQUEUE_ABI_VERSION` in the UAPI header.
