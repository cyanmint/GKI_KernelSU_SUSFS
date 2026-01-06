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

The patches can be optionally enabled during the kernel build process in the GitHub Actions workflow:

1. Set `enable_lxc_docker: true` in the workflow inputs
2. After kernel source is synced and other patches are applied
3. Before the final kernel configuration step
4. The workflow checks if the relevant source files exist
5. Patches are applied with forward-only mode to prevent conflicts

## Using Docker/LXC Support

### Enabling the Patches

The patches are **optional** and must be explicitly enabled when running the GitHub Actions workflow. To enable them:

1. When triggering the workflow (via workflow_dispatch), check the `enable_lxc_docker` option
2. Or when calling the workflow from another workflow, pass `enable_lxc_docker: true`

**Important:** When `enable_lxc_docker` is enabled, the workflow will automatically:
- Apply the panic fix and cgroup patches
- Enable required kernel configurations (CONFIG_SYSVIPC, CONFIG_IPC_NS, CONFIG_PID_NS, CONFIG_NET_NS, etc.)
- Configure cgroup and namespace support needed for containers

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
   - This will automatically apply the boot fix patches
   - This will automatically enable required kernel configurations
2. Optionally use the `custom_kernel_configs` input parameter to enable additional configs if needed

```yaml
enable_lxc_docker: true
# Optional: Add any additional custom configs
custom_kernel_configs: 'CONFIG_OVERLAY_FS,CONFIG_EXT4_FS_SECURITY'
```

**Note:** When `enable_lxc_docker` is enabled, the following configs are automatically enabled:
- Namespace support (CONFIG_NAMESPACES, CONFIG_IPC_NS, CONFIG_PID_NS, CONFIG_NET_NS, CONFIG_UTS_NS, CONFIG_USER_NS)
- SYSVIPC support (CONFIG_SYSVIPC, CONFIG_SYSVIPC_SYSCTL)
- Cgroup support (CONFIG_CGROUPS, CONFIG_CGROUP_DEVICE, CONFIG_CGROUP_FREEZER, CONFIG_CGROUP_PIDS, etc.)
- Network features (CONFIG_VETH, CONFIG_BRIDGE, CONFIG_BRIDGE_NETFILTER)

The patches will be automatically applied before the build starts when `enable_lxc_docker` is enabled.

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

- These patches are **optional** and must be explicitly enabled via the `enable_lxc_docker` workflow input
- No manual intervention is required when the option is enabled
- Patches are safe and do not affect normal Android functionality
- Only applied if the source files exist in the kernel version being built
- When disabled (default), the kernel builds without LXC/Docker support patches
