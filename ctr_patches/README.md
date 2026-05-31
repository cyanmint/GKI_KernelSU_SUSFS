# Containerd Patches for Android GKI Kernels

This directory contains Containerd support patches organized by Android version and kernel version.

## Directory Structure

```
ctr_patches/
├── a12-5.10/  # Android 12 - Kernel 5.10
├── a13-5.10/  # Android 13 - Kernel 5.10
├── a13-5.15/  # Android 13 - Kernel 5.15
├── a14-5.15/  # Android 14 - Kernel 5.15
├── a14-6.1/   # Android 14 - Kernel 6.1
├── a15-6.6/   # Android 15 - Kernel 6.6
└── a16-6.12/  # Android 16 - Kernel 6.12
```

## Patches Source

Original patches from: https://github.com/tomxi1997/Enable-LXC-Dockers-for-Android-GKI-kernel  
Based on work by: https://github.com/lateautumn233/Common-Android-Kernel-Tree

## Kernel Source Branches

These patches are tested against the following kernel branches from https://android.googlesource.com/kernel/common:

- `android12-5.10-2025-02` for a12-5.10
- `android13-5.10-2025-01` for a13-5.10
- `android13-5.15-2025-01` for a13-5.15
- `android14-5.15-2025-01` for a14-5.15
- `android14-6.1-2025-01` for a14-6.1
- `android15-6.6-2025-01` for a15-6.6
- `android16-6.12-2025-06` for a16-6.12

## Required Kernel Configurations

All containerd patches require these kernel configurations (automatically applied during build):

### Namespaces & IPC
- `CONFIG_SYSVIPC=y`
- `CONFIG_POSIX_MQUEUE=y`
- `CONFIG_UTS_NS=y`
- `CONFIG_PID_NS=y`
- `CONFIG_IPC_NS=y`
- `CONFIG_USER_NS=y`
- `CONFIG_NET_NS=y`

### Cgroups
- `CONFIG_CGROUP_DEVICE=y`
- `CONFIG_CGROUP_FREEZER=y`

### Networking
- `CONFIG_NETFILTER_XT_TARGET_CHECKSUM=y`
- `CONFIG_NETFILTER_XT_MATCH_ADDRTYPE=y`
- `CONFIG_IP6_NF_NAT=y`
- `CONFIG_IP6_NF_TARGET_MASQUERADE=y`

### Device Support
- `CONFIG_DEVTMPFS=y`
- `CONFIG_NULL_TTY=y`

### Build Configuration
- `CONFIG_LTO_CLANG_THIN=y`

### Configs to Remove
- `CONFIG_LTO_CLANG_FULL`
- `CONFIG_MODULE_SCMVERSION`
- `CONFIG_VT` restrictions
- `CONFIG_PID_NS is not set`

## Patch Application

Patches are applied automatically during the kernel build process. The build system:
1. Detects the Android version and kernel version being built
2. Selects the appropriate patch directory
3. Applies all `.patch` files in order
4. Configures the required kernel options

## Credits

- [lateautumn233](https://github.com/lateautumn233) - Original containerd patches
- [tomxi1997](https://github.com/tomxi1997) - Patch repository maintenance
- [TheKit](https://github.com/TheKit) - GKI ABI padding patches
- Community contributors
