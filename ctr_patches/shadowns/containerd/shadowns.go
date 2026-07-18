// SPDX-License-Identifier: GPL-2.0
//
// Package shadowns is a reference client for the shadowns simulated namespace
// subsystem exposed by the shadowns kernel module at /dev/shadowns.
//
// It is intended to be vendored into a *patched* containerd/runc build. Where
// upstream containerd would ask the OCI runtime to create Linux namespaces via
// clone(2)/unshare(2)/setns(2), a patched runtime can instead (or additionally)
// drive shadow namespaces through this client so that namespace bookkeeping and
// UTS isolation work on kernels built without the native CONFIG_*_NS options.
//
// The ABI implemented here mirrors ctr_patches/shadowns/include/uapi/shadowns.h
// and must be kept in sync with SHADOWNS_ABI_VERSION.
package shadowns

import (
	"fmt"
	"os"
	"unsafe"

	"golang.org/x/sys/unix"
)

// Device path and ABI version, mirroring the kernel UAPI header.
const (
	DevicePath = "/dev/shadowns"
	ABIVersion = 1

	utsLen = 64
)

// Type identifies a shadow namespace kind. Values match enum shadowns_type.
type Type uint32

const (
	TypeUTS    Type = 0
	TypeIPC    Type = 1
	TypeMNT    Type = 2
	TypePID    Type = 3
	TypeNET    Type = 4
	TypeUSER   Type = 5
	TypeCGROUP Type = 6
	TypeAny    Type = 7 // SHADOWNS_TYPE_MAX: "accept any type" for SetNS
)

// ioctl request numbers. These are computed to match the _IOR/_IOW/_IOWR macros
// in the kernel UAPI header (magic 'S').
var (
	iocABIVersion = iorw('S', 0, 4, dirRead)                            // __u32
	iocCreate     = iorw('S', 1, int(unsafe.Sizeof(createReq{})), dirRW) // struct shadowns_create
	iocUnshare    = iorw('S', 2, int(unsafe.Sizeof(createReq{})), dirRW)
	iocSetNS      = iorw('S', 3, int(unsafe.Sizeof(setnsReq{})), dirWrite)
	iocGet        = iorw('S', 4, int(unsafe.Sizeof(getReq{})), dirRW)
	iocDestroy    = iorw('S', 5, 4, dirWrite) // __u32
	iocSetUTS     = iorw('S', 6, int(unsafe.Sizeof(utsReq{})), dirWrite)
	iocGetUTS     = iorw('S', 7, int(unsafe.Sizeof(utsReq{})), dirRead)
)

const (
	dirNone  = 0
	dirWrite = 1 // _IOC_WRITE: userspace -> kernel
	dirRead  = 2 // _IOC_READ:  kernel -> userspace
	dirRW    = dirWrite | dirRead
)

// iorw builds an ioctl request number using the standard asm-generic encoding
// (8-bit size field at bit 0, type at 8, nr at 0-ish...). The layout is:
//   dir(2) | size(14) | type(8) | nr(8)
func iorw(typ, nr, size, dir int) uintptr {
	const (
		nrbits   = 8
		typebits = 8
		sizebits = 14
		nrshift  = 0
		typeshift = nrshift + nrbits
		sizeshift = typeshift + typebits
		dirshift  = sizeshift + sizebits
	)
	return uintptr(dir)<<dirshift |
		uintptr(typ)<<typeshift |
		uintptr(nr)<<nrshift |
		uintptr(size)<<sizeshift
}

type createReq struct {
	Type     uint32
	Flags    uint32
	ID       uint32
	ParentID uint32
}

type setnsReq struct {
	ID   uint32
	Type uint32
}

type getReq struct {
	Type uint32
	ID   uint32
}

type utsReq struct {
	Nodename   [utsLen + 1]byte
	Domainname [utsLen + 1]byte
}

// Session wraps an open handle to /dev/shadowns. Each Session corresponds to one
// open file descriptor; all shadow namespace references it holds are released by
// the kernel when the Session is closed, so a crashed runtime never leaks state.
type Session struct {
	f *os.File
}

// Open opens the shadowns device and verifies the ABI version.
func Open() (*Session, error) {
	f, err := os.OpenFile(DevicePath, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("shadowns: open %s: %w", DevicePath, err)
	}
	s := &Session{f: f}
	ver, err := s.ABIVersion()
	if err != nil {
		f.Close()
		return nil, err
	}
	if ver != ABIVersion {
		f.Close()
		return nil, fmt.Errorf("shadowns: ABI mismatch: kernel=%d client=%d", ver, ABIVersion)
	}
	return s, nil
}

// Close releases the session and every shadow namespace reference it holds.
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
		return 0, fmt.Errorf("shadowns: abi version: %w", err)
	}
	return v, nil
}

// Create allocates a detached shadow namespace of the given type and returns its
// id. The session retains the reference until Destroy or Close.
func (s *Session) Create(t Type) (id uint32, parent uint32, err error) {
	req := createReq{Type: uint32(t)}
	if err := s.ioctl(iocCreate, unsafe.Pointer(&req)); err != nil {
		return 0, 0, fmt.Errorf("shadowns: create type=%d: %w", t, err)
	}
	return req.ID, req.ParentID, nil
}

// Unshare creates a new shadow namespace of the given type derived from the
// session's current namespace of that type, then joins it (mirrors unshare(2)).
func (s *Session) Unshare(t Type) (id uint32, parent uint32, err error) {
	req := createReq{Type: uint32(t)}
	if err := s.ioctl(iocUnshare, unsafe.Pointer(&req)); err != nil {
		return 0, 0, fmt.Errorf("shadowns: unshare type=%d: %w", t, err)
	}
	return req.ID, req.ParentID, nil
}

// SetNS joins an existing shadow namespace by id (mirrors setns(2)). Pass
// TypeAny to skip type validation, or the expected Type to enforce it.
func (s *Session) SetNS(id uint32, expect Type) error {
	req := setnsReq{ID: id, Type: uint32(expect)}
	if err := s.ioctl(iocSetNS, unsafe.Pointer(&req)); err != nil {
		return fmt.Errorf("shadowns: setns id=%d: %w", id, err)
	}
	return nil
}

// Get returns the session's current shadow namespace id for a type, or 0.
func (s *Session) Get(t Type) (uint32, error) {
	req := getReq{Type: uint32(t)}
	if err := s.ioctl(iocGet, unsafe.Pointer(&req)); err != nil {
		return 0, fmt.Errorf("shadowns: get type=%d: %w", t, err)
	}
	return req.ID, nil
}

// Destroy drops the caller's reference to a shadow namespace id.
func (s *Session) Destroy(id uint32) error {
	if err := s.ioctl(iocDestroy, unsafe.Pointer(&id)); err != nil {
		return fmt.Errorf("shadowns: destroy id=%d: %w", id, err)
	}
	return nil
}

// SetHostname sets nodename/domainname on the session's current UTS shadow
// namespace. The session must have joined a UTS namespace (via Unshare/SetNS).
func (s *Session) SetHostname(nodename, domainname string) error {
	var req utsReq
	copyStr(req.Nodename[:], nodename)
	copyStr(req.Domainname[:], domainname)
	if err := s.ioctl(iocSetUTS, unsafe.Pointer(&req)); err != nil {
		return fmt.Errorf("shadowns: set uts: %w", err)
	}
	return nil
}

// Hostname reads nodename/domainname from the current UTS shadow namespace.
func (s *Session) Hostname() (nodename, domainname string, err error) {
	var req utsReq
	if err := s.ioctl(iocGetUTS, unsafe.Pointer(&req)); err != nil {
		return "", "", fmt.Errorf("shadowns: get uts: %w", err)
	}
	return cstr(req.Nodename[:]), cstr(req.Domainname[:]), nil
}

func copyStr(dst []byte, s string) {
	n := copy(dst, s)
	if n < len(dst) {
		dst[n] = 0
	} else {
		dst[len(dst)-1] = 0
	}
}

func cstr(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}
