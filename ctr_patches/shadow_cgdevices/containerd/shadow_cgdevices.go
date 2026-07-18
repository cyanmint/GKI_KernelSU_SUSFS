// SPDX-License-Identifier: GPL-2.0
//
// Package shadowcgdevices is a reference client for the shadow_cgdevices
// simulated cgroup device controller exposed by the shadow_cgdevices kernel
// module at /dev/shadow_cgdevices.
//
// It is intended to be vendored into a *patched* containerd/runc build. Where
// a runtime would normally write to devices.allow / devices.deny in the cgroup
// hierarchy to configure per-container device access, a patched runtime can
// instead manage virtual "shadow cgroups" through this client on kernels built
// without CONFIG_CGROUP_DEVICE.
//
// The ABI implemented here mirrors
// ctr_patches/shadow_cgdevices/include/uapi/shadow_cgdevices.h and must be
// kept in sync with SHADOW_CGDEV_ABI_VERSION.
package shadowcgdevices

import (
	"fmt"
	"os"
	"unsafe"

	"golang.org/x/sys/unix"
)

// Device path and ABI version, mirroring the kernel UAPI header.
const (
	DevicePath = "/dev/shadow_cgdevices"
	ABIVersion = 1
)

// Device type codes matching the cgroup v1 device controller convention.
const (
	TypeAll   = 'a'
	TypeBlock = 'b'
	TypeChar  = 'c'
)

// Access permission bits matching the cgroup v1 convention.
const (
	AccessRead  = 0x01
	AccessWrite = 0x02
	AccessMknod = 0x04
)

// WildcardMajor / WildcardMinor match any major/minor number in a rule.
const (
	WildcardMajor = int32(-1)
	WildcardMinor = int32(-1)
)

// ioctl request numbers.
var (
	iocABIVersion = iorw('G', 0, 4, dirRead)
	iocCreate     = iorw('G', 1, int(unsafe.Sizeof(createReq{})), dirRW)
	iocDestroy    = iorw('G', 2, 4, dirWrite) // __u32
	iocRuleAdd    = iorw('G', 3, int(unsafe.Sizeof(ruleReq{})), dirWrite)
	iocRuleReset  = iorw('G', 4, 4, dirWrite) // __u32
	iocCheck      = iorw('G', 5, int(unsafe.Sizeof(checkReq{})), dirRW)
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

// createReq mirrors struct shadow_cgdev_create.
type createReq struct {
	ParentID uint32
	ID       uint32
}

// ruleReq mirrors struct shadow_cgdev_rule.
type ruleReq struct {
	CgroupID uint32
	DevType  uint8
	Access   uint8
	Allow    uint8
	_pad     uint8
	Major    int32
	Minor    int32
}

// checkReq mirrors struct shadow_cgdev_check.
type checkReq struct {
	CgroupID uint32
	DevType  uint8
	Access   uint8
	Allowed  uint8
	_pad     uint8
	Major    int32
	Minor    int32
}

// Session wraps an open handle to /dev/shadow_cgdevices.
type Session struct {
	f *os.File
}

// Open opens the shadow_cgdevices device and verifies the ABI version.
func Open() (*Session, error) {
	f, err := os.OpenFile(DevicePath, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("shadow_cgdevices: open %s: %w", DevicePath, err)
	}
	s := &Session{f: f}
	ver, err := s.ABIVersion()
	if err != nil {
		f.Close()
		return nil, err
	}
	if ver != ABIVersion {
		f.Close()
		return nil, fmt.Errorf("shadow_cgdevices: ABI mismatch: kernel=%d client=%d", ver, ABIVersion)
	}
	return s, nil
}

// Close releases the session and every shadow cgroup it owns.
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
		return 0, fmt.Errorf("shadow_cgdevices: abi version: %w", err)
	}
	return v, nil
}

// Create creates a new virtual shadow cgroup as a child of parentID (0 = root).
// Returns the new cgroup id.
func (s *Session) Create(parentID uint32) (id uint32, err error) {
	req := createReq{ParentID: parentID}
	if err := s.ioctl(iocCreate, unsafe.Pointer(&req)); err != nil {
		return 0, fmt.Errorf("shadow_cgdevices: create parent=%d: %w", parentID, err)
	}
	return req.ID, nil
}

// Destroy releases the session's ownership reference to a shadow cgroup.
func (s *Session) Destroy(id uint32) error {
	if err := s.ioctl(iocDestroy, unsafe.Pointer(&id)); err != nil {
		return fmt.Errorf("shadow_cgdevices: destroy id=%d: %w", id, err)
	}
	return nil
}

// Allow appends an "allow" device rule to the given shadow cgroup.
// major/minor: use WildcardMajor/WildcardMinor (-1) for wildcard.
// access: OR of AccessRead, AccessWrite, AccessMknod.
func (s *Session) Allow(cgID uint32, devType byte, major, minor int32, access byte) error {
	req := ruleReq{
		CgroupID: cgID,
		DevType:  devType,
		Access:   access,
		Allow:    1,
		Major:    major,
		Minor:    minor,
	}
	if err := s.ioctl(iocRuleAdd, unsafe.Pointer(&req)); err != nil {
		return fmt.Errorf("shadow_cgdevices: allow cg=%d %c %d:%d %#x: %w",
			cgID, devType, major, minor, access, err)
	}
	return nil
}

// Deny appends a "deny" device rule to the given shadow cgroup.
func (s *Session) Deny(cgID uint32, devType byte, major, minor int32, access byte) error {
	req := ruleReq{
		CgroupID: cgID,
		DevType:  devType,
		Access:   access,
		Allow:    0,
		Major:    major,
		Minor:    minor,
	}
	if err := s.ioctl(iocRuleAdd, unsafe.Pointer(&req)); err != nil {
		return fmt.Errorf("shadow_cgdevices: deny cg=%d %c %d:%d %#x: %w",
			cgID, devType, major, minor, access, err)
	}
	return nil
}

// Reset removes all device rules from the given shadow cgroup, restoring the
// default deny-all state.
func (s *Session) Reset(cgID uint32) error {
	if err := s.ioctl(iocRuleReset, unsafe.Pointer(&cgID)); err != nil {
		return fmt.Errorf("shadow_cgdevices: reset cg=%d: %w", cgID, err)
	}
	return nil
}

// Check queries whether a device access is permitted by the shadow cgroup's
// current rule list.  devType must be TypeBlock or TypeChar.
func (s *Session) Check(cgID uint32, devType byte, major, minor int32, access byte) (bool, error) {
	req := checkReq{
		CgroupID: cgID,
		DevType:  devType,
		Access:   access,
		Major:    major,
		Minor:    minor,
	}
	if err := s.ioctl(iocCheck, unsafe.Pointer(&req)); err != nil {
		return false, fmt.Errorf("shadow_cgdevices: check cg=%d %c %d:%d %#x: %w",
			cgID, devType, major, minor, access, err)
	}
	return req.Allowed != 0, nil
}
