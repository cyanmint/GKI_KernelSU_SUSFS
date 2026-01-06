# LXC/Docker Support for Android GKI Kernels

This directory contains patches and configuration files required to enable LXC and Docker support on Android GKI kernels without causing boot failures.

## Problem

Enabling `CONFIG_SYSVIPC` and related IPC namespace configurations (required for Docker/LXC) can cause kernel panic during boot in the `xt_qtaguid` netfilter module.

## Solution

Two critical patches are applied during kernel build:

### 1. fix_panic.patch
Fixes kernel panic in `net/netfilter/xt_qtaguid.c` by preventing access to network device statistics that can cause crashes when namespaces are enabled.

### 2. fix_cgroup.patch
Adds cgroup compatibility fixes for Docker/LXC containers in `kernel/cgroup/cgroup.c`.

### 3. Kconfig
Provides a menuconfig option to enable all required Docker/LXC kernel configurations.

## Usage

These patches are automatically applied during the kernel build process when Docker/LXC support is enabled.

## Credits

Based on patches from:
- https://github.com/tomxi1997/lxc-docker-support-for-android
