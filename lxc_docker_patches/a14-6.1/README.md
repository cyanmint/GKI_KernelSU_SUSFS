# Android 14 (6.1) LXC/Docker Patches

## Kernel Source
Branch: `android14-6.1-2025-01` from https://android.googlesource.com/kernel/common

## Description
These patches enable LXC and Docker container support on Android 14 GKI kernels (6.1). They modify the kernel to support container namespaces, overlayfs, and other necessary features while maintaining ABI compatibility.

## What These Patches Do

1. **gki_use_Android_ABI_padding_for_SYSVIPC_task_struct_fields.patch** - Use Android ABI padding for SYSVIPC task_struct fields
   - Enables `CONFIG_SYSVIPC=y` without breaking module ABI compatibility

2. **overlayfs_dont_make_DCACHE_OP_HASH_and_DCACHE_OP_COMPARE_weird.patch** - Remove overlayfs DCACHE_OP_{HASH,COMPARE} check
   - Fixes overlayfs compatibility with case-insensitive filesystems (required for modern Android)

3. **Ignore_symbols_crc_check.patch** - Ignore module symbol CRC check
   - Allows loading modules with different symbol CRCs

4. **cgroup_fix_cgroup_prefix.patch** - Fix cgroup prefix
   - Fixes cgroup naming to ensure proper container operation

5. **gki_use_Android_ABI_padding_for_CGROUP_DEVICE_dev_cgroup_fields.patch** - Use Android ABI padding for CGROUP_DEVICE dev_cgroup fields
   - Enables `CONFIG_CGROUP_DEVICE=y` without breaking module ABI compatibility
   - **CRITICAL FIX**: Prevents kernel panic and bootloop on android14-6.1.118
   - Ensures proper cgroup device controller initialization

6. **gki_use_Android_ABI_padding_for_cgroup_subsys_struct.patch** - Use Android ABI padding for cgroup_subsys struct
   - Adds ABI padding to the cgroup_subsys structure
   - **CRITICAL FIX**: Prevents kernel panic when CONFIG_CGROUP_DEVICE is enabled
   - Ensures cgroup subsystem array stability across config changes

7. **gki_use_Android_ABI_padding_for_ipc_namespace_struct.patch** - Use Android ABI padding for ipc_namespace struct
   - Adds ABI padding to the ipc_namespace structure
   - **CRITICAL FIX**: Prevents ABI breakage when CONFIG_SYSVIPC is enabled
   - Complements task_struct padding for complete SYSVIPC ABI stability

## Debugging Support

See **DEBUGGING_GUIDE.md** for comprehensive information on:
- Enabling kernel debugging features
- Capturing kernel logs and panic messages
- Using pstore/ramoops for persistent logging
- Recommended kernel config options for debugging
- How to access logs after boot failure

## Credits
- [lateautumn233](https://github.com/lateautumn233) - Original patch development
- [tomxi1997](https://github.com/tomxi1997) - LXC/Docker patches repository
