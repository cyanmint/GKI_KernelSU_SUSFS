// SPDX-License-Identifier: GPL-2.0
//
// Package shadowmqueue is a reference client for the shadow_mqueue simulated
// POSIX message queue subsystem exposed by the shadow_mqueue kernel module at
// /dev/shadow_mqueue.
//
// It is intended to be vendored into a *patched* containerd/runc build. Where
// runc would normally use mq_open/mq_send/mq_receive (or their Go wrappers)
// for parent↔child synchronisation during container initialisation, a patched
// runtime can drive the shadow_mqueue module through this client on kernels
// built without CONFIG_POSIX_MQUEUE.
//
// Message transfer is fully functional: bytes written via Send are buffered in
// the kernel and delivered to Receive in priority order, with blocking and
// timeout semantics mirroring POSIX mq_send(3)/mq_receive(3).
//
// The ABI implemented here mirrors
// ctr_patches/shadow_mqueue/include/uapi/shadow_mqueue.h and must be kept in
// sync with SHADOW_MQUEUE_ABI_VERSION.
package shadowmqueue

import (
	"fmt"
	"math"
	"os"
	"unsafe"

	"golang.org/x/sys/unix"
)

// Device path and ABI version, mirroring the kernel UAPI header.
const (
	DevicePath = "/dev/shadow_mqueue"
	ABIVersion = 1

	NameMax    = 255
	MsgSizeMax = 256
	MaxMsgDef  = 10
	MaxMsgMax  = 1024
)

// Open flags.
const (
	O_CREAT    = 0x01
	O_EXCL     = 0x02
	O_NONBLOCK = 0x04
	O_RDONLY   = 0x08
	O_WRONLY   = 0x10
	O_RDWR     = 0x18
)

// BlockForever is the timeout value for indefinitely blocking Send/Receive.
const BlockForever = uint64(math.MaxUint64)

// Attr mirrors struct shadow_mq_attr.
type Attr struct {
	Flags   int64
	MaxMsg  int64
	MsgSize int64
	CurMsgs int64
}

// ioctl request numbers.
var (
	iocABIVersion = iorw('Q', 0, 4, dirRead)
	iocOpen       = iorw('Q', 1, int(unsafe.Sizeof(openReq{})), dirRW)
	iocClose      = iorw('Q', 2, int(unsafe.Sizeof(closeReq{})), dirWrite)
	iocUnlink     = iorw('Q', 3, int(unsafe.Sizeof(unlinkReq{})), dirWrite)
	iocSend       = iorw('Q', 4, int(unsafe.Sizeof(sendReq{})), dirWrite)
	iocReceive    = iorw('Q', 5, int(unsafe.Sizeof(recvReq{})), dirRW)
	iocGetattr    = iorw('Q', 6, int(unsafe.Sizeof(attrReq{})), dirRW)
	iocSetattr    = iorw('Q', 7, int(unsafe.Sizeof(attrReq{})), dirRW)
)

const (
	dirNone  = 0
	dirWrite = 1
	dirRead  = 2
	dirRW    = dirWrite | dirRead
)

func iorw(typ, nr, size, dir int) uintptr {
	const (
		nrshift   = 0
		typeshift = 8
		sizeshift = 16
		dirshift  = 30
	)
	return uintptr(dir)<<dirshift |
		uintptr(size)<<sizeshift |
		uintptr(typ)<<typeshift |
		uintptr(nr)<<nrshift
}

// attrWire mirrors struct shadow_mq_attr.
type attrWire struct {
	Flags   int64
	MaxMsg  int64
	MsgSize int64
	CurMsgs int64
}

// openReq mirrors struct shadow_mq_open_req.
type openReq struct {
	Name   [NameMax + 1]byte
	Oflag  uint32
	Mode   uint32
	Attr   attrWire
	Handle uint32
	_pad   uint32
}

// closeReq mirrors struct shadow_mq_close_req.
type closeReq struct {
	Handle uint32
	_pad   uint32
}

// unlinkReq mirrors struct shadow_mq_unlink_req.
type unlinkReq struct {
	Name [NameMax + 1]byte
	_pad [3]byte
}

// sendReq mirrors struct shadow_mq_send_req.
type sendReq struct {
	Handle    uint32
	Prio      uint32
	TimeoutNs uint64
	MsgLen    uint32
	_pad      uint32
	MsgData   [MsgSizeMax]byte
}

// recvReq mirrors struct shadow_mq_recv_req.
type recvReq struct {
	Handle    uint32
	Prio      uint32
	TimeoutNs uint64
	MsgLen    uint32
	_pad      uint32
	MsgData   [MsgSizeMax]byte
}

// attrReq mirrors struct shadow_mq_attr_req.
type attrReq struct {
	Handle  uint32
	_pad    uint32
	NewAttr attrWire
	Attr    attrWire
}

// Session wraps an open handle to /dev/shadow_mqueue.
type Session struct {
	f *os.File
}

// Open opens the shadow_mqueue device and verifies the ABI version.
func Open() (*Session, error) {
	f, err := os.OpenFile(DevicePath, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("shadow_mqueue: open %s: %w", DevicePath, err)
	}
	s := &Session{f: f}
	ver, err := s.ABIVersion()
	if err != nil {
		f.Close()
		return nil, err
	}
	if ver != ABIVersion {
		f.Close()
		return nil, fmt.Errorf("shadow_mqueue: ABI mismatch: kernel=%d client=%d", ver, ABIVersion)
	}
	return s, nil
}

// Close releases the session and all queue handles it holds.
func (s *Session) Close() error { return s.f.Close() }

func (s *Session) ioctl(req uintptr, arg unsafe.Pointer) error {
	_, _, errno := unix.Syscall(unix.SYS_IOCTL, s.f.Fd(), req, uintptr(arg))
	if errno != 0 {
		return errno
	}
	return nil
}

// ABIVersion returns the ABI version implemented by the loaded module.
func (s *Session) ABIVersion() (uint32, error) {
	var v uint32
	if err := s.ioctl(iocABIVersion, unsafe.Pointer(&v)); err != nil {
		return 0, fmt.Errorf("shadow_mqueue: abi version: %w", err)
	}
	return v, nil
}

// MQOpen opens (and optionally creates) a named message queue.
//
// name must start with '/'. oflag is a combination of O_CREAT, O_EXCL,
// O_NONBLOCK, O_RDONLY/O_WRONLY/O_RDWR. attr specifies queue attributes when
// creating; pass nil for defaults.
//
// Returns a per-session handle and the actual queue attributes.
func (s *Session) MQOpen(name string, oflag uint32, mode uint32, attr *Attr) (handle uint32, actual Attr, err error) {
	var req openReq
	copyName(req.Name[:], name)
	req.Oflag = oflag
	req.Mode  = mode
	if attr != nil {
		req.Attr.Flags   = attr.Flags
		req.Attr.MaxMsg  = attr.MaxMsg
		req.Attr.MsgSize = attr.MsgSize
	}
	if err := s.ioctl(iocOpen, unsafe.Pointer(&req)); err != nil {
		return 0, Attr{}, fmt.Errorf("shadow_mqueue: open %q: %w", name, err)
	}
	actual = Attr{
		Flags:   req.Attr.Flags,
		MaxMsg:  req.Attr.MaxMsg,
		MsgSize: req.Attr.MsgSize,
		CurMsgs: req.Attr.CurMsgs,
	}
	return req.Handle, actual, nil
}

// MQClose closes a per-session queue handle.
func (s *Session) MQClose(handle uint32) error {
	req := closeReq{Handle: handle}
	if err := s.ioctl(iocClose, unsafe.Pointer(&req)); err != nil {
		return fmt.Errorf("shadow_mqueue: close handle=%d: %w", handle, err)
	}
	return nil
}

// MQUnlink removes a named message queue.
func (s *Session) MQUnlink(name string) error {
	var req unlinkReq
	copyName(req.Name[:], name)
	if err := s.ioctl(iocUnlink, unsafe.Pointer(&req)); err != nil {
		return fmt.Errorf("shadow_mqueue: unlink %q: %w", name, err)
	}
	return nil
}

// Send sends msg to the queue identified by handle with the given priority.
// timeoutNs controls blocking: 0 = non-blocking, BlockForever = no timeout,
// other = wait at most that many nanoseconds.
func (s *Session) Send(handle uint32, prio uint32, timeoutNs uint64, msg []byte) error {
	if len(msg) == 0 || len(msg) > MsgSizeMax {
		return fmt.Errorf("shadow_mqueue: send: message length %d out of range [1,%d]", len(msg), MsgSizeMax)
	}
	var req sendReq
	req.Handle    = handle
	req.Prio      = prio
	req.TimeoutNs = timeoutNs
	req.MsgLen    = uint32(len(msg))
	copy(req.MsgData[:], msg)
	if err := s.ioctl(iocSend, unsafe.Pointer(&req)); err != nil {
		return fmt.Errorf("shadow_mqueue: send handle=%d: %w", handle, err)
	}
	return nil
}

// Receive receives a message from the queue identified by handle.
// timeoutNs has the same semantics as Send.
// Returns the message bytes and its priority.
func (s *Session) Receive(handle uint32, timeoutNs uint64) (msg []byte, prio uint32, err error) {
	var req recvReq
	req.Handle    = handle
	req.TimeoutNs = timeoutNs
	req.MsgLen    = MsgSizeMax
	if err := s.ioctl(iocReceive, unsafe.Pointer(&req)); err != nil {
		return nil, 0, fmt.Errorf("shadow_mqueue: receive handle=%d: %w", handle, err)
	}
	out := make([]byte, req.MsgLen)
	copy(out, req.MsgData[:req.MsgLen])
	return out, req.Prio, nil
}

// GetAttr returns the current attributes of the queue identified by handle.
func (s *Session) GetAttr(handle uint32) (Attr, error) {
	req := attrReq{Handle: handle}
	if err := s.ioctl(iocGetattr, unsafe.Pointer(&req)); err != nil {
		return Attr{}, fmt.Errorf("shadow_mqueue: getattr handle=%d: %w", handle, err)
	}
	return Attr{
		Flags:   req.Attr.Flags,
		MaxMsg:  req.Attr.MaxMsg,
		MsgSize: req.Attr.MsgSize,
		CurMsgs: req.Attr.CurMsgs,
	}, nil
}

// SetAttr sets the NONBLOCK flag on the queue (the only writable attribute).
// Returns the previous attributes.
func (s *Session) SetAttr(handle uint32, newFlags int64) (old Attr, err error) {
	req := attrReq{Handle: handle}
	req.NewAttr.Flags = newFlags
	if err := s.ioctl(iocSetattr, unsafe.Pointer(&req)); err != nil {
		return Attr{}, fmt.Errorf("shadow_mqueue: setattr handle=%d: %w", handle, err)
	}
	return Attr{
		Flags:   req.Attr.Flags,
		MaxMsg:  req.Attr.MaxMsg,
		MsgSize: req.Attr.MsgSize,
		CurMsgs: req.Attr.CurMsgs,
	}, nil
}

func copyName(dst []byte, name string) {
	n := copy(dst, name)
	if n < len(dst) {
		dst[n] = 0
	} else {
		dst[len(dst)-1] = 0
	}
}
