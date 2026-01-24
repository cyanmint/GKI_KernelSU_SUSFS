# Android 16 (6.12) LXC/Docker Patches

## Kernel Source
Branch: `android-mainline` from https://android.googlesource.com/kernel/common

## Description
These patches enable LXC and Docker container support on Android 16 GKI kernels (6.12). They modify the kernel to support container namespaces, overlayfs, and other necessary features while maintaining ABI compatibility.

## What These Patches Do

1. **0ac686b9e81ba331c2ad9b420fd21262a80daaa4.patch** - Use Android ABI padding for SYSVIPC task_struct fields
   - Enables `CONFIG_SYSVIPC=y` without breaking module ABI compatibility

2. **3dcc884c689681dda2d9ad24a9e219013f70cfe8.patch** - Remove overlayfs DCACHE_OP_{HASH,COMPARE} check
   - Fixes overlayfs compatibility with case-insensitive filesystems (required for modern Android)

3. **750b43051d2e4317121c7250544ae38fdf28d4c7.patch** - Ignore module symbol CRC check
   - Allows loading modules with different symbol CRCs

4. **a72032ecf33c63d8a4abb64b08c1a0b847c82a32.patch** - Fix cgroup prefix
   - Fixes cgroup naming to ensure proper container operation

5. **e8f6c8d4b2a9f1e3c5d7a6b8e9f2c4d1a3b5e7f9.patch** - Use Android ABI padding for CGROUP_DEVICE dev_cgroup fields
   - Enables `CONFIG_CGROUP_DEVICE=y` without breaking module ABI compatibility
   - **CRITICAL FIX**: Prevents kernel panic and bootloop
   - Ensures proper cgroup device controller initialization

## Credits
- [lateautumn233](https://github.com/lateautumn233) - Original patch development
- [tomxi1997](https://github.com/tomxi1997) - LXC/Docker patches repository
