# Android 13 (5.10) containerd Patches

## Kernel Source
Branch: `deprecated/android13-5.10-2025-01` from https://android.googlesource.com/kernel/common

## Description
These patches enable LXC and Docker container support on Android 13 GKI kernels (5.10). They modify the kernel to support container namespaces, overlayfs, and other necessary features while maintaining ABI compatibility.

## Patch Categories

### Core Containerd Patches (Always Applied)

1. **gki_use_Android_ABI_padding_for_SYSVIPC_task_struct_fields.patch** - Use Android ABI padding for SYSVIPC task_struct fields
   - Enables `CONFIG_SYSVIPC=y` without breaking module ABI compatibility

2. **overlayfs_dont_make_DCACHE_OP_HASH_and_DCACHE_OP_COMPARE_weird.patch** - Remove overlayfs DCACHE_OP_{HASH,COMPARE} check
   - Fixes overlayfs compatibility with case-insensitive filesystems (required for modern Android)

3. **Ignore_symbols_crc_check.patch** - Ignore module symbol CRC check
   - Allows loading modules with different symbol CRCs

4. **cgroup_fix_cgroup_prefix.patch** - Fix cgroup prefix
   - Fixes cgroup naming to ensure proper container operation

5. **device_cgroup_safety_fix.patch** - Add NULL safety check in device cgroup legacy permission path
   - Avoids potential early-path NULL dereference in legacy permission checks
   - Keeps upstream cgroup subsystem behavior unchanged

6. **gki_use_Android_ABI_padding_for_cgroup_subsys_struct.patch** - Use Android ABI padding for cgroup_subsys struct
   - Adds ABI padding to the cgroup_subsys structure
   - **CRITICAL FIX**: Prevents kernel panic when CONFIG_CGROUP_DEVICE is enabled
   - Ensures cgroup subsystem array stability across config changes

7. **gki_use_Android_ABI_padding_for_ipc_namespace_struct.patch** - Use Android ABI padding for ipc_namespace struct
   - Adds ABI padding to the ipc_namespace structure
   - **CRITICAL FIX**: Prevents ABI breakage when CONFIG_SYSVIPC is enabled
   - Complements task_struct padding for complete SYSVIPC ABI stability

8. **cgroup_subsys_guarded_devices.patch** - Keep devices cgroup subsystem guarded by `CONFIG_CGROUP_DEVICE`
   - Preserves upstream subsystem registration behavior
   - Adds clarifying comment only; does not force subsystem ID changes

### Diagnostic Patches (Automatically Applied)

9. **kernel/exit.c.debug.patch** - Enhanced init exit diagnostics
   - Provides detailed diagnostic messages when init exits
   - Helps identify root cause of boot loops (e.g., missing fstab)
   - No functional changes, only improved error messages
   - See `../INIT_EXIT_DIAGNOSTICS.md` for details

## How It Works

These patches work together to enable CONFIG_CGROUP_DEVICE without breaking the kernel:

1. **ABI Padding Patches (1-7)** keep key container features compatible with Android GKI constraints
2. **Cgroup safety/compatibility patch (8)** keeps upstream subsystem gating behavior and avoids forced enum/layout shifts
3. The kernel uses standard BUG_ON() for critical errors - if something goes wrong, it will fail fast with a panic
4. With proper ABI padding and subsystem count stability, errors shouldn't occur in the first place
5. If you see boot issues, check kernel logs via pstore/ramoops to diagnose the actual problem

**Important**: Do not add patches that replace BUG_ON with WARN_ON in cgroup initialization. Such patches cause boot hangs by leaving subsystems in partially initialized states.

## Debugging Support

See **DEBUGGING_GUIDE.md** for comprehensive information on:
- Enabling kernel debugging features
- Capturing kernel logs and panic messages
- Using pstore/ramoops for persistent logging
- Recommended kernel config options for debugging
- How to access logs after boot failure

## Credits
- [lateautumn233](https://github.com/lateautumn233) - Original patch development
- [tomxi1997](https://github.com/tomxi1997) - containerd patches repository
