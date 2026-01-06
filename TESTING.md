# Testing Guide for CONFIG_SYSVIPC Boot Failure Fix

## Overview
This guide explains how to test the CONFIG_SYSVIPC boot failure fix in this repository.

## Automatic Testing (via GitHub Actions)

The patches are automatically applied during the GitHub Actions build process. To test:

### 1. Enable CONFIG_SYSVIPC via custom_kernel_configs

Trigger a workflow with:
```yaml
custom_kernel_configs: 'CONFIG_SYSVIPC,CONFIG_IPC_NS,CONFIG_PID_NS,CONFIG_NET_NS'
```

### 2. Check Build Logs

In the GitHub Actions logs, look for:
```
应用 LXC/Docker 支持补丁
应用 xt_qtaguid 修复补丁以防止 CONFIG_SYSVIPC 相关的内核 panic...
xt_qtaguid panic fix applied successfully
应用 cgroup 修复补丁以支持 Docker/LXC...
cgroup fix applied successfully
```

### 3. Flash and Boot Test

After the kernel builds successfully:
1. Download the kernel image (AnyKernel3.zip or boot.img)
2. Flash it to your device
3. Device should boot normally without kernel panic

## Manual Testing (Local Build)

If building locally:

### 1. Apply Patches Manually

```bash
cd kernel_source/common
patch -p1 < /path/to/lxc_docker_support/fix_panic.patch
patch -p1 < /path/to/lxc_docker_support/fix_cgroup.patch
```

### 2. Enable IPC Configs

Add to your defconfig:
```
CONFIG_SYSVIPC=y
CONFIG_IPC_NS=y
CONFIG_PID_NS=y
CONFIG_NET_NS=y
CONFIG_UTS_NS=y
CONFIG_USER_NS=y
```

### 3. Build and Test

```bash
make -j$(nproc)
# Flash the kernel
# Boot and verify
```

## Verification Steps

After booting the patched kernel:

### 1. Check Boot Success
Device should boot to Android without kernel panic

### 2. Check Kernel Logs
```bash
adb shell dmesg | grep -i qtaguid
adb shell dmesg | grep -i panic
```
Should not show any panics related to xt_qtaguid

### 3. Verify IPC Support
```bash
adb shell cat /proc/sys/kernel/shmmax
adb shell ls -la /proc/sys/kernel/sem
```
Should show IPC parameters are available

### 4. Test Namespace Support
```bash
adb shell ls -la /proc/self/ns/
```
Should show ipc, pid, net, uts, and user namespaces

### 5. Optional: Test LXC/Docker (if fully configured)
If you enabled the full Docker configuration:
```bash
# Install LXC/Docker binaries
# Try creating a container
# Container should start without kernel panic
```

## Expected Results

### ✅ Success Indicators:
- Device boots normally
- No kernel panic in logs
- IPC syscalls work: `shmmax`, `sem` available
- Namespaces are present in `/proc/self/ns/`
- (Optional) LXC/Docker containers can start

### ❌ Failure Indicators:
- Device fails to boot (bootloop)
- Kernel panic in `xt_qtaguid.c`
- Missing IPC parameters
- Missing namespaces in `/proc`

## Troubleshooting

### Problem: Patches don't apply
Check the build logs for:
```
WARNING: xt_qtaguid panic fix not applied (already patched or conflicts exist)
```

This could mean:
1. Patches already applied (safe to ignore)
2. Conflict with other patches (needs manual resolution)
3. Kernel version doesn't have xt_qtaguid.c (safe to ignore for newer kernels)

### Problem: Device still doesn't boot
1. Check kernel logs: `adb shell cat /sys/fs/pstore/console-ramoops`
2. Look for different panic reason
3. May need additional patches for your specific kernel version

### Problem: Namespaces still not working
1. Verify all configs are enabled: `zcat /proc/config.gz | grep -E "IPC_NS|PID_NS|NET_NS"`
2. Check SELinux policies (may block namespace creation)
3. Verify init system supports namespaces

## Reporting Issues

If you encounter problems:
1. Capture full kernel boot log
2. Note your kernel version (android version + kernel version)
3. Include device model
4. Specify which configs you enabled
5. Open an issue with all details

## Additional Notes

- The fix is primarily for kernels that have xt_qtaguid module
- Newer GKI kernels may not need these patches (xt_qtaguid removed)
- Patches are safe to apply even if not strictly needed
- The workflow automatically detects and skips if files don't exist
