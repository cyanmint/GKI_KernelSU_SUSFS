# Docker/LXC Patches for Android GKI Kernels

This directory contains patches to enable Docker and LXC container support on Android GKI kernels.

## Patch Descriptions

### Universal Patches (Apply to all kernel versions)

1. **0ac686b9e81ba331c2ad9b420fd21262a80daaa4.patch**
   - Subject: GKI: use Android ABI padding for SYSVIPC task_struct fields
   - Purpose: Allows CONFIG_SYSVIPC=y without breaking module ABI
   - File: `include/linux/sched.h`

2. **3dcc884c689681dda2d9ad24a9e219013f70cfe8.patch**
   - Subject: overlayfs: don't make DCACHE_OP_{HASH,COMPARE} weird
   - Purpose: Fix overlayfs operations for container support
   - File: `fs/overlayfs/*`

3. **596330385b5f8545be462be7889b640647b31610.patch**
   - Subject: kernel: Use the stock config for /proc/config.gz
   - Purpose: Preserve kernel config in /proc/config.gz
   - File: `arch/arm64/configs/stock_gki_defconfig`

4. **a0aa446ca326b5d26ac1dec057efd8c07d2bcbff.patch**
   - Subject: GKI: use Android ABI padding for POSIX_MQUEUE user_struct
   - Purpose: Allows CONFIG_POSIX_MQUEUE=y without breaking module ABI
   - File: `include/linux/sched/user.h`
   - **Note**: May not apply cleanly to all kernel versions due to context changes

5. **a72032ecf33c63d8a4abb64b08c1a0b847c82a32.patch**
   - Subject: cgroup: fix cgroup prefix
   - Purpose: Fix cgroup naming for container support
   - File: `kernel/cgroup/cgroup.c`

### Version-Specific Patches

1. **750b43051d2e4317121c7250544ae38fdf28d4c7.patch**
   - Subject: kernel/module.c: Ignore symbols crc check
   - Purpose: Ignore CRC checks for kernel modules
   - File: `kernel/module.c`
   - **Compatibility**: Only for kernels < 6.1
   - **Reason**: In kernel 6.1+, `kernel/module.c` was refactored into `kernel/module/` directory

## Patch Application

The workflow automatically:
1. Checks if target files exist before applying patches
2. Skips patches when target files don't exist (e.g., 750b4305... on kernel 6.1+)
3. Attempts multiple application strategies:
   - `git apply` (clean apply)
   - `patch -p1` (standard)
   - `patch -p1 --fuzz=3 --forward` (tolerant of context changes)
4. Continues build even if some patches fail to apply

## Source

These patches are originally from: https://github.com/tomxi1997/enable-lxc-dockers-for-android-gki-kernel

## Updating Patches

When kernel sources are updated and patches no longer apply:
1. Test each patch against the new kernel version
2. Update patches that fail to apply due to context changes
3. Create version-specific patches if needed
4. Update this README with compatibility information
