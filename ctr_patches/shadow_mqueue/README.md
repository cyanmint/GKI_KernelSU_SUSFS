# shadow_mqueue — simulated POSIX message queue subsystem

`shadow_mqueue` is a **standalone loadable kernel module** that provides fully
functional POSIX message queues through ioctls on `/dev/shadow_mqueue`. A
**patched containerd/runc** can use this module to send and receive messages on
a GKI kernel built **without** `CONFIG_POSIX_MQUEUE`.

## Why this exists

The native POSIX mqueue subsystem (`CONFIG_POSIX_MQUEUE`) is compiled **directly
into `vmlinux`** and mounts the `mqueue` filesystem.  It cannot be added by a
module after the kernel is built.

`runc` uses POSIX message queues for **parent↔child synchronisation** during
container initialisation (the "init pipe").  Without `CONFIG_POSIX_MQUEUE` that
synchronisation fails and containers never start.

`shadow_mqueue` provides a fully functional drop-in: message transfer is real—
bytes written via SEND are buffered in a kernel-side priority list and delivered
to RECEIVE in order, with blocking and timeout semantics that mirror
`mq_send(3)`/`mq_receive(3)`.

## Architecture

```
  patched runc (parent)                    shadow_mqueue.ko
  ┌─────────────────────────┐  ioctl  ┌────────────────────────────────┐
  │ shadow_mqueue.go client │────────▶│ /dev/shadow_mqueue misc device │
  │  Open()                 │         │  per-fd "session"               │
  │  MQOpen("/runc-init", …)│         │   handles[] -> mq_handle_entry  │
  │  Send(handle, msg)      │         │  global name -> shadow_mq       │
  │  Receive(handle, …)     │         │   (refcounted, wait queues)     │
  │  MQClose(handle)        │         └────────────────────────────────┘
  │  MQUnlink("/runc-init") │
  └─────────────────────────┘

  patched runc (child) uses the same session pattern on its side of the pipe.
```

* **Session = open fd.**  Each `open("/dev/shadow_mqueue")` is one session.
  All handles and their buffered messages are released on `close()`.
* **Named queues** persist in a global hash table until `MQUnlink`.  Two
  sessions can share a queue by name.
* **Blocking** is supported via `wait_queue_head_t`; receivers sleep until a
  message arrives (or a timeout fires); senders sleep if the queue is full.
* **Priority ordering**: messages are stored highest-priority-first, matching
  POSIX semantics.

## What is simulated

| Operation           | Behaviour                                                        |
|---------------------|------------------------------------------------------------------|
| `MQOpen`            | Create or get a named queue; returns per-session handle.         |
| `Send`              | Buffer a message; block or timeout if queue is full.             |
| `Receive`           | Dequeue the highest-priority message; block or timeout if empty. |
| `MQClose`           | Drop the session's reference to the queue.                       |
| `MQUnlink`          | Remove the queue from the name table; wake all blocked waiters.  |
| `GetAttr`/`SetAttr` | Read/write NONBLOCK flag; read mq_maxmsg/mq_msgsize/mq_curmsgs.  |

### Honest scope / limitations

* `SHADOW_MQ_MSGSIZE_MAX` (256 bytes) and `SHADOW_MQ_MAXMSG_MAX` (1024 msgs)
  are sufficient for runc's init sync but smaller than the POSIX defaults for
  general-purpose mqueues.  Adjust the header constants and recompile if larger
  messages are needed.
* There is no `mq_notify` (signal/thread notification on message arrival).
* Blocking operations inside ioctls use `wait_event_interruptible_timeout`,
  which rounds the nanosecond timeout down to the next jiffy boundary (≈ 4 ms
  on HZ=250 kernels).

## Files

| Path                                    | Purpose                                    |
|-----------------------------------------|--------------------------------------------|
| `include/uapi/shadow_mqueue.h`          | Stable ioctl ABI shared with userspace.    |
| `shadow_mqueue.c`                       | The kernel module.                         |
| `Makefile`                              | Out-of-tree build (`make KDIR=...`).       |
| `containerd/shadow_mqueue.go`           | Reference Go client for containerd/runc.   |
| `containerd/README.md`                  | Runtime integration notes.                 |

## Building

```sh
cd ctr_patches/shadow_mqueue
make KDIR=/path/to/kernel/build
sudo insmod shadow_mqueue.ko
ls -l /dev/shadow_mqueue
```

For an Android GKI cross build:
```sh
make -C /path/to/kernel/build M=$(pwd) ARCH=arm64 LLVM=1 modules
```

## Loading

```sh
insmod shadow_mqueue.ko      # creates /dev/shadow_mqueue (mode 0600)
dmesg | grep shadow_mqueue   # "simulated POSIX mqueue subsystem loaded (ABI v1)"
```

## ABI versioning

`SHADOW_MQUEUE_IOC_ABI_VERSION` returns `SHADOW_MQUEUE_ABI_VERSION`.  Clients
must verify it on open.  Bump in `include/uapi/shadow_mqueue.h` whenever the
ioctl layout changes.
