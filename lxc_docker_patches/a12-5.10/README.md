# Android 12 (5.10) LXC/Docker Patches

## Kernel Source
Branch: `android12-5.10-2025-02` from https://android.googlesource.com/kernel/common

## Description
These patches enable LXC and Docker container support on Android 12 GKI kernels (5.10). They modify the kernel to support container namespaces, overlayfs, and other necessary features while maintaining ABI compatibility.

## What These Patches Do

1. **0ac686b9e81ba331c2ad9b420fd21262a80daaa4.patch** - Use Android ABI padding for SYSVIPC task_struct fields
   - Enables `CONFIG_SYSVIPC=y` without breaking module ABI compatibility

2. **3dcc884c689681dda2d9ad24a9e219013f70cfe8.patch** - Remove overlayfs DCACHE_OP_{HASH,COMPARE} check
   - Fixes overlayfs compatibility with case-insensitive filesystems (required for modern Android)

3. **596330385b5f8545be462be7889b640647b31610.patch** - Use stock config for /proc/config.gz
   - Ensures config visibility matches stock kernel configuration

4. **750b43051d2e4317121c7250544ae38fdf28d4c7.patch** - Ignore module symbol CRC check
   - Allows loading modules with different symbol CRCs

5. **a0aa446ca326b5d26ac1dec057efd8c07d2bcbff.patch** - Use Android ABI padding for POSIX_MQUEUE user_struct fields
   - Enables `CONFIG_POSIX_MQUEUE=y` without breaking module ABI compatibility

6. **a72032ecf33c63d8a4abb64b08c1a0b847c82a32.patch** - Fix cgroup prefix
   - Fixes cgroup naming to ensure proper container operation

## Credits
- [lateautumn233](https://github.com/lateautumn233) - Original patch development
- [tomxi1997](https://github.com/tomxi1997) - LXC/Docker patches repository
