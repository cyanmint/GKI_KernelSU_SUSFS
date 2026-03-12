# Android 12 (5.10) LXC/Docker Patches

## Kernel Source
Branch: `android12-5.10-2025-02` from https://android.googlesource.com/kernel/common

## Description
These patches enable LXC and Docker container support on Android 12 GKI kernels (5.10). They modify the kernel to support container namespaces, overlayfs, and other necessary features while maintaining ABI compatibility.

## What These Patches Do

1. **gki_use_Android_ABI_padding_for_SYSVIPC_task_struct_fields.patch** - Use Android ABI padding for SYSVIPC task_struct fields
   - Enables `CONFIG_SYSVIPC=y` without breaking module ABI compatibility

2. **overlayfs_dont_make_DCACHE_OP_HASH_and_DCACHE_OP_COMPARE_weird.patch** - Remove overlayfs DCACHE_OP_{HASH,COMPARE} check
   - Fixes overlayfs compatibility with case-insensitive filesystems (required for modern Android)

3. **596330385b5f8545be462be7889b640647b31610.patch** - Use stock config for /proc/config.gz
   - Ensures config visibility matches stock kernel configuration

4. **Ignore_symbols_crc_check.patch** - Ignore module symbol CRC check
   - Allows loading modules with different symbol CRCs

5. **GKI_use_Android_ABI_padding_for_POSIX_MQUEUE_user_struct_fields.patch** - Use Android ABI padding for POSIX_MQUEUE user_struct fields
   - Enables `CONFIG_POSIX_MQUEUE=y` without breaking module ABI compatibility

6. **cgroup_fix_cgroup_prefix.patch** - Fix cgroup prefix
   - Fixes cgroup naming to ensure proper container operation

## Credits
- [lateautumn233](https://github.com/lateautumn233) - Original patch development
- [tomxi1997](https://github.com/tomxi1997) - LXC/Docker patches repository
