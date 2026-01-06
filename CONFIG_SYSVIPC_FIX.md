# CONFIG_SYSVIPC Boot Failure Fix

## Problem

When enabling `CONFIG_SYSVIPC` and related IPC namespace configurations (required for Docker/LXC support) in the Android GKI kernel, the device may fail to boot with a kernel panic. The build process completes successfully, but the kernel may crash during boot.

## Root Cause

The boot failure can be caused by a kernel panic in the `xt_qtaguid` netfilter module (`net/netfilter/xt_qtaguid.c`) on older kernel versions (5.10, 5.15). When IPC namespaces are enabled, the module may attempt to access network device statistics that are not available or safe to access during early boot, resulting in a null pointer dereference.

**Note:** Newer kernel versions (6.1+, 6.6+) do not have the `xt_qtaguid` module and are not affected by this specific issue.

## Solution

This repository includes **experimental** patches vendored from https://github.com/tomxi1997/lxc-docker-support-for-android with safer modifications:

### Patches Applied

1. **fix_panic_safe.patch** - Safer panic prevention in xt_qtaguid
   - Adds null pointer check before accessing net_dev
   - Preserves network statistics for valid devices
   - Only uses fallback stats when device is invalid
   - **Much safer than the original aggressive patch**

2. **fix_cgroup.patch** - DISABLED (causes boot loops)
   - Original patch modifies cgroup file creation
   - Known to cause boot loops on many devices
   - Not applied by default

### How It Works

The patches can be optionally enabled during the kernel build process in the GitHub Actions workflow:

1. Set `enable_lxc_docker: true` in the workflow inputs
2. After kernel source is synced and other patches are applied
3. Only the safe panic fix is applied (cgroup patch is skipped)
4. Required kernel configs are automatically enabled
5. Patches are applied with forward-only mode to prevent conflicts

## ⚠️ WARNINGS

**EXPERIMENTAL FEATURE - USE AT YOUR OWN RISK**

- These patches are experimental and may cause boot loops on some devices
- The cgroup patch is disabled by default due to known issues
- Even with the safer panic fix, there may be compatibility issues
- Newer kernels (6.1+, 6.6+) may not need these patches at all
- Full Docker/LXC support may require additional manual modifications

**Only enable this feature if:**
- You understand the risks
- You can recover from boot loops
- You have a backup of your working kernel
- You're willing to test and debug issues

## Using Docker/LXC Support

### Enabling the Patches

The patches are **EXPERIMENTAL** and must be explicitly enabled when running the GitHub Actions workflow. To enable them:

1. When triggering the workflow (via workflow_dispatch), check the `enable_lxc_docker` option
2. Or when calling the workflow from another workflow, pass `enable_lxc_docker: true`

**Important:** When `enable_lxc_docker` is enabled, the workflow will automatically:
- Apply the safer xt_qtaguid panic fix patch (if xt_qtaguid.c exists)
- Skip the cgroup patch (known to cause boot loops)
- Enable required kernel configurations (CONFIG_SYSVIPC, CONFIG_IPC_NS, CONFIG_PID_NS, CONFIG_NET_NS, etc.)
- Configure cgroup and namespace support needed for containers

**⚠️ WARNING:** Even with the safer patches, boot loops may still occur. Only enable if you can recover from boot failures.

The patches will only be applied if this option is enabled.

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

# SECURITY WARNING: Disabling CONFIG_ANDROID_PARANOID_NETWORK removes network access
# restrictions that normally limit which apps can use networking features. This is
# required for containers to function, but on production Android devices it may allow
# untrusted apps to access the network. Only disable this on devices where you have
# full control and understand the security implications.
```

The boot panic fixes are automatically applied, so these configurations are now safe to use.

### Option 3: Using GitHub Actions Workflow

When triggering the GitHub Actions workflow, you can:

1. Enable the LXC/Docker patches by checking the `enable_lxc_docker` option
   - This will apply only the safer xt_qtaguid panic fix (cgroup patch is disabled)
   - This will automatically enable required kernel configurations
2. Optionally use the `custom_kernel_configs` input parameter to enable additional configs if needed

```yaml
enable_lxc_docker: true
# Optional: Add any additional custom configs
custom_kernel_configs: 'CONFIG_OVERLAY_FS,CONFIG_EXT4_FS_SECURITY'
```

**⚠️ IMPORTANT WARNINGS:**
- **EXPERIMENTAL**: This feature is experimental and may cause boot loops
- **NOT GUARANTEED**: Even with safer patches, some devices may not boot
- **CGROUP PATCH DISABLED**: The cgroup patch is not applied due to known boot loop issues
- **LIMITED FUNCTIONALITY**: Without the cgroup patch, full Docker/LXC support may not work
- **MANUAL WORK NEEDED**: Full Docker/LXC support may require additional manual kernel modifications

**Note:** When `enable_lxc_docker` is enabled, the following configs are automatically enabled:
- Namespace support (CONFIG_NAMESPACES, CONFIG_IPC_NS, CONFIG_PID_NS, CONFIG_NET_NS, CONFIG_UTS_NS, CONFIG_USER_NS)
- SYSVIPC support (CONFIG_SYSVIPC, CONFIG_SYSVIPC_SYSCTL)
- Cgroup support (CONFIG_CGROUPS, CONFIG_CGROUP_DEVICE, CONFIG_CGROUP_FREEZER, CONFIG_CGROUP_PIDS, etc.)
- Network features (CONFIG_VETH, CONFIG_BRIDGE, CONFIG_BRIDGE_NETFILTER)

Only the safer panic fix patch will be applied when `enable_lxc_docker` is enabled.

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

- These patches are **EXPERIMENTAL** and may still cause boot loops on some devices
- The cgroup patch is disabled by default due to known boot loop issues
- Only the safer xt_qtaguid panic fix is applied when `enable_lxc_docker` is enabled
- Patches are safe and intended to minimize impact on Android functionality
- Only applied if the source files exist in the kernel version being built
- When disabled (default), the kernel builds without LXC/Docker support patches
- Full Docker/LXC support may require additional manual modifications beyond these patches

## Vendored Source

The patches are vendored from https://github.com/tomxi1997/lxc-docker-support-for-android with safer modifications. See `lxc_docker_support_upstream/VENDOR.md` for details.
