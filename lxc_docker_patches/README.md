# LXC Docker Patches for Android GKI Kernel

These patches enable LXC and Docker support for Android GKI kernels.

## Source

Patches are vendored from: https://github.com/tomxi1997/Enable-LXC-Dockers-for-Android-GKI-kernel

Original source: https://github.com/lateautumn233/Common-Android-Kernel-Tree

## Required Kernel Configurations

The following kernel configs are required for LXC/Docker support and are automatically applied during the build process:

### Configs to Add:
- `CONFIG_SYSVIPC=y`
- `CONFIG_POSIX_MQUEUE=y`
- `CONFIG_CGROUP_DEVICE=y`
- `CONFIG_CGROUP_FREEZER=y`
- `CONFIG_UTS_NS=y`
- `CONFIG_PID_NS=y`
- `CONFIG_IPC_NS=y`
- `CONFIG_USER_NS=y`
- `CONFIG_NET_NS=y`
- `CONFIG_NETFILTER_XT_TARGET_CHECKSUM=y`
- `CONFIG_NETFILTER_XT_MATCH_ADDRTYPE=y`
- `CONFIG_IP6_NF_NAT=y`
- `CONFIG_IP6_NF_TARGET_MASQUERADE=y`
- `CONFIG_RFKILL=y`
- `CONFIG_DEVTMPFS=y`
- `CONFIG_NULL_TTY=y`

### Configs to Remove/Disable:
- `CONFIG_LTO_CLANG_FULL` (replaced with `CONFIG_LTO_CLANG_THIN=y`)
- `CONFIG_MODULE_SCMVERSION`
- `CONFIG_VT`

## Patches Included

1. `0ac686b9e81ba331c2ad9b420fd21262a80daaa4.patch`
2. `3dcc884c689681dda2d9ad24a9e219013f70cfe8.patch`
3. `596330385b5f8545be462be7889b640647b31610.patch`
4. `750b43051d2e4317121c7250544ae38fdf28d4c7.patch`
5. `a0aa446ca326b5d26ac1dec057efd8c07d2bcbff.patch`
6. `a72032ecf33c63d8a4abb64b08c1a0b847c82a32.patch`

## Credits

- [lateautumn233](https://github.com/lateautumn233) - Original patches
- [tomxi1997](https://github.com/tomxi1997) - LXC Docker patch repository
