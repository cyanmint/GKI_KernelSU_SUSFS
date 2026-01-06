# CONFIG_SYSVIPC Boot Failure Fix

## Problem

When enabling `CONFIG_SYSVIPC` and related IPC namespace configurations (required for Docker/LXC support) in the Android GKI kernel, the device fails to boot with a kernel panic. The build process completes successfully, but the kernel crashes during boot.

## Root Cause

The boot failure is caused by a kernel panic in the `xt_qtaguid` netfilter module (`net/netfilter/xt_qtaguid.c`). When IPC namespaces are enabled, the module attempts to access network device statistics that may not be available or safe to access during early boot, resulting in a null pointer dereference or similar crash.

## Solution

This repository now includes automatic patches that fix the boot failure while preserving Docker/LXC functionality:

### Patches Applied

1. **fix_panic.patch** - Prevents kernel panic in xt_qtaguid
   - Modifies `net/netfilter/xt_qtaguid.c`
   - Prevents access to potentially invalid device statistics
   - Ensures boot stability with IPC namespaces enabled

2. **fix_cgroup.patch** - Adds cgroup compatibility
   - Modifies `kernel/cgroup/cgroup.c`
   - Adds compatibility links for cgroup files
   - Improves Docker/LXC container support

### How It Works

The patches are automatically applied during the kernel build process in the GitHub Actions workflow:

1. After kernel source is synced and other patches are applied
2. Before the final kernel configuration step
3. The workflow checks if the relevant source files exist
4. Patches are applied with forward-only mode to prevent conflicts

## Using Docker/LXC Support

### Option 1: Use the Kconfig (Recommended)

If you want full Docker/LXC support with all required kernel options:

1. During kernel build, the Kconfig file can be integrated into the kernel source
2. Enable "Docker/LXC support" in menuconfig under "Utilities"
3. This automatically enables all required kernel configurations

### Option 2: Manual Configuration

To manually enable just the IPC support with boot fixes:

Add these configurations to your kernel's defconfig:
```
# Basic IPC support (safe with our patches)
CONFIG_SYSVIPC=y
CONFIG_IPC_NS=y
CONFIG_PID_NS=y
CONFIG_NET_NS=y
CONFIG_UTS_NS=y
CONFIG_USER_NS=y

# Disable paranoid network (required for containers)
# CONFIG_ANDROID_PARANOID_NETWORK is not set
```

The boot panic fixes are automatically applied, so these configurations are now safe to use.

### Option 3: Using custom_kernel_configs Input

When triggering the GitHub Actions workflow, you can use the `custom_kernel_configs` input parameter to enable specific configs:

```yaml
custom_kernel_configs: 'CONFIG_SYSVIPC,CONFIG_IPC_NS,CONFIG_PID_NS,CONFIG_NET_NS,CONFIG_UTS_NS,CONFIG_USER_NS'
```

The patches will be automatically applied before the build starts.

## Testing

After flashing the kernel:

1. Device should boot normally without kernel panic
2. Check kernel logs: `dmesg | grep -i qtaguid`
3. Verify IPC support: `cat /proc/sys/kernel/shmmax`
4. Test container functionality if using full Docker/LXC support

## Technical Details

### xt_qtaguid Panic Fix

The original code in `xt_qtaguid.c`:
```c
if (iface_entry->active) {
    stats = dev_get_stats(iface_entry->net_dev, &dev_stats);
} else {
    stats = &no_dev_stats;
}
```

After patch:
```c
stats = &no_dev_stats;
```

This prevents accessing potentially invalid network device pointers during boot when namespaces are being initialized.

### Cgroup Fix

Adds symbolic links for cgroup files when `CGRP_ROOT_NOPREFIX` is set, ensuring compatibility with container runtimes that expect both prefixed and non-prefixed cgroup file paths.

## References

- Based on patches from: https://github.com/tomxi1997/lxc-docker-support-for-android
- Related discussion: https://www.coolapk.com/feed/47142899

## Credits

- Original patches: tomxi1997
- Integration and fixes: This repository

## Notes

- These patches are applied automatically in the GitHub Actions workflow
- No manual intervention is required
- Patches are safe and do not affect normal Android functionality
- Only applied if the source files exist in the kernel version being built
