// SPDX-License-Identifier: GPL-2.0
//
// Package shadowsysvipc is a reference client for the shadow_sysvipc simulated
// System V IPC subsystem exposed by the shadow_sysvipc kernel module at
// /dev/shadow_sysvipc.
//
// It is intended to be vendored into a *patched* containerd/runc build. Where
// a runtime would normally call msgget(2)/semget(2)/shmget(2) to create SysV
// IPC resources, a patched runtime can instead drive virtual IPC objects
// through this client on kernels built without CONFIG_SYSVIPC.
//
// The ABI implemented here mirrors
// ctr_patches/shadow_sysvipc/include/uapi/shadow_sysvipc.h and must be kept in
// sync with SHADOW_SYSVIPC_ABI_VERSION.
package shadowsysvipc

import (
	"fmt"
	"os"
	"unsafe"

	"golang.org/x/sys/unix"
)

// Device path and ABI version, mirroring the kernel UAPI header.
const (
	DevicePath = "/dev/shadow_sysvipc"
	ABIVersion = 1

	// IPC_PRIVATE requests a private (non-keyed) resource.
	IPC_PRIVATE = 0
)

// ResourceType identifies a virtual IPC object kind.
type ResourceType uint32

const (
	TypeMsgQ ResourceType = 0 // message queue
	TypeSem  ResourceType = 1 // semaphore set
	TypeShm  ResourceType = 2 // shared-memory segment
)

// Flags for Create, mirroring the kernel IPC_CREAT / IPC_EXCL values.
const (
	IPC_CREAT = 0x0200
	IPC_EXCL  = 0x0400
)

// ioctl request numbers, computed to match _IOR/_IOW/_IOWR(magic='V', ...).
var (
	iocABIVersion = iorw('V', 0, 4, dirRead)                               // __u32
	iocCreate     = iorw('V', 1, int(unsafe.Sizeof(createReq{})), dirRW)   // struct shadow_sysvipc_create
	iocStat       = iorw('V', 2, int(unsafe.Sizeof(statReq{})), dirRW)     // struct shadow_sysvipc_stat
	iocDestroy    = iorw('V', 3, int(unsafe.Sizeof(destroyReq{})), dirWrite) // struct shadow_sysvipc_destroy
)

const (
	dirNone  = 0
	dirWrite = 1
	dirRead  = 2
	dirRW    = dirWrite | dirRead
)

// iorw encodes a Linux ioctl request number: dir(2)|size(14)|type(8)|nr(8).
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

// createReq mirrors struct shadow_sysvipc_create.
type createReq struct {
	Type  uint32
	Flags uint32
	Key   int32
	Nsems uint32
	Size  uint64
	ID    uint32
	_pad  uint32
}

// statReq mirrors struct shadow_sysvipc_stat.
type statReq struct {
	Type  uint32
	ID    uint32
	Key   int32
	Flags uint32
	Nsems uint32
	_pad  uint32
	Size  uint64
}

// destroyReq mirrors struct shadow_sysvipc_destroy.
type destroyReq struct {
	Type uint32
	ID   uint32
}

// Session wraps an open handle to /dev/shadow_sysvipc.  Each Session
// corresponds to one open file descriptor; all virtual IPC resources it holds
// are released by the kernel when the Session is closed.
type Session struct {
	f *os.File
}

// Open opens the shadow_sysvipc device and verifies the ABI version.
func Open() (*Session, error) {
	f, err := os.OpenFile(DevicePath, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("shadow_sysvipc: open %s: %w", DevicePath, err)
	}
	s := &Session{f: f}
	ver, err := s.ABIVersion()
	if err != nil {
		f.Close()
		return nil, err
	}
	if ver != ABIVersion {
		f.Close()
		return nil, fmt.Errorf("shadow_sysvipc: ABI mismatch: kernel=%d client=%d", ver, ABIVersion)
	}
	return s, nil
}

// Close releases the session and every virtual IPC resource reference it holds.
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
		return 0, fmt.Errorf("shadow_sysvipc: abi version: %w", err)
	}
	return v, nil
}

// Create creates or gets a virtual IPC resource.
//
// t is the resource type (TypeMsgQ, TypeSem, TypeShm).
// key is SHADOW_IPC_PRIVATE for a new private resource, or an integer key for
// shared resources (same key semantics as msgget/semget/shmget).
// flags is IPC_CREAT | IPC_EXCL | permission-bits.
// nsems is the semaphore count (TypeSem only); size is the segment size in
// bytes (TypeShm only).
//
// Returns the stable resource id.
func (s *Session) Create(t ResourceType, key int32, flags uint32, nsems uint32, size uint64) (id uint32, err error) {
	req := createReq{
		Type:  uint32(t),
		Flags: flags,
		Key:   key,
		Nsems: nsems,
		Size:  size,
	}
	if err := s.ioctl(iocCreate, unsafe.Pointer(&req)); err != nil {
		return 0, fmt.Errorf("shadow_sysvipc: create type=%d key=%d: %w", t, key, err)
	}
	return req.ID, nil
}

// Stat queries metadata about a virtual IPC resource by id.
func (s *Session) Stat(t ResourceType, id uint32) (key int32, flags, nsems uint32, size uint64, err error) {
	req := statReq{Type: uint32(t), ID: id}
	if err := s.ioctl(iocStat, unsafe.Pointer(&req)); err != nil {
		return 0, 0, 0, 0, fmt.Errorf("shadow_sysvipc: stat type=%d id=%d: %w", t, id, err)
	}
	return req.Key, req.Flags, req.Nsems, req.Size, nil
}

// Destroy releases the session's ownership reference to a virtual IPC resource.
// The resource is freed when its last reference is dropped.
func (s *Session) Destroy(t ResourceType, id uint32) error {
	req := destroyReq{Type: uint32(t), ID: id}
	if err := s.ioctl(iocDestroy, unsafe.Pointer(&req)); err != nil {
		return fmt.Errorf("shadow_sysvipc: destroy type=%d id=%d: %w", t, id, err)
	}
	return nil
}
