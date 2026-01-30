# Kernel Debugging Configuration for Android 14-6.1

## Overview

This document explains how to enable kernel debugging features to diagnose cgroup initialization failures and other boot issues.

## Debugging Patches

### 1. cgroup_replace_BUG_ON_with_WARN_ON_for_debugging.patch

**Purpose**: Prevents kernel panic on cgroup errors, allowing the system to continue and capture error logs.

**Changes**:
- Replaces `BUG_ON()` calls with `WARN_ON()` in cgroup initialization
- Adds error logging before returning from failed operations
- Allows system to attempt continuing boot even with cgroup errors

**Benefits**:
- System doesn't immediately reboot on cgroup errors
- Error messages are logged and can be captured
- Debugging information remains available for analysis

## Recommended Kernel Config Options for Debugging

Add these to your kernel configuration to enable comprehensive debugging:

```kconfig
# Enable verbose logging
CONFIG_PRINTK=y
CONFIG_PRINTK_TIME=y
CONFIG_LOG_BUF_SHIFT=21  # 2MB log buffer (default is usually smaller)

# Enable early printk (platform-specific, adjust as needed)
CONFIG_EARLY_PRINTK=y

# Enable kernel debugging
CONFIG_DEBUG_KERNEL=y
CONFIG_DEBUG_INFO=y
CONFIG_FRAME_WARN=2048

# Enable panic behavior that helps debugging
CONFIG_PANIC_ON_OOPS=n  # Don't panic on oops, allow logging
CONFIG_PANIC_TIMEOUT=0  # Don't auto-reboot on panic

# Enable pstore for persistent logging
CONFIG_PSTORE=y
CONFIG_PSTORE_CONSOLE=y
CONFIG_PSTORE_PMSG=y
CONFIG_PSTORE_RAM=y

# Enable ftrace for function tracing
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y
CONFIG_STACK_TRACER=y

# Enable KASAN for memory debugging (if needed, adds overhead)
# CONFIG_KASAN=y

# Cgroup debugging
CONFIG_CGROUP_DEBUG=y
```

## Kernel Command Line Options

Add these to your bootloader (bootconfig or cmdline):

```
# Enable console output
console=ttyMSM0,115200 console=tty0

# Enable early printk (platform-specific)
earlyprintk=serial,ttyMSM0,115200

# Set log level to maximum verbosity
loglevel=8 debug

# Disable quiet mode
androidboot.selinux=permissive

# Enable panic behavior
panic=0 softlockup_panic=0 hung_task_panic=0

# Pstore configuration
ramoops.mem_address=0x9ff00000
ramoops.mem_size=0x100000
ramoops.record_size=0x40000
ramoops.console_size=0x40000
ramoops.pmsg_size=0x40000
```

**Note**: Adjust memory addresses and console devices for your specific platform.

## Accessing Logs After Boot Failure

### Method 1: ADB (if system boots enough)

```bash
adb shell dmesg > kernel.log
adb shell cat /proc/last_kmsg > last_kmsg.log
adb pull /sys/fs/pstore/ pstore_logs/
```

### Method 2: Pstore (after reboot)

```bash
adb shell
mount -t pstore pstore /sys/fs/pstore
cat /sys/fs/pstore/console-ramoops-0
cat /sys/fs/pstore/dmesg-ramoops-*
```

### Method 3: UART Console

Connect a UART/serial console to your device to see kernel messages in real-time.

## Debugging Workflow

1. **Apply the cgroup debugging patch**:
   - This prevents immediate panic on cgroup errors
   - Allows system to continue attempting boot

2. **Enable verbose logging**:
   - Add kernel config options above
   - Add command line parameters

3. **Capture logs**:
   - Connect UART console if available
   - Enable pstore for persistent logging
   - Use adb to pull logs after boot

4. **Analyze errors**:
   - Look for WARN_ON messages in dmesg
   - Check cgroup initialization sequence
   - Identify which subsystem is failing

## Common Issues and Solutions

### Issue: No console output

**Solution**: 
- Verify console device name for your platform (ttyMSM0, ttyS0, etc.)
- Enable earlyprintk
- Check if console is muxed with another interface

### Issue: Logs disappear after reboot

**Solution**:
- Enable pstore/ramoops
- Verify pstore memory region doesn't conflict with other allocations
- Check pstore mounts after reboot

### Issue: System still panics

**Solution**:
- Ensure cgroup_replace_BUG_ON_with_WARN_ON patch is applied
- Check if panic is from different subsystem
- Review panic backtrace to identify source

## Testing the Patches

1. Apply patches to kernel source:
```bash
cd kernel_source
patch -p1 < lxc_docker_patches/a14-6.1/cgroup_replace_BUG_ON_with_WARN_ON_for_debugging.patch
```

2. Enable debugging configs in defconfig

3. Build and flash kernel

4. Monitor boot process via UART or capture logs

5. Analyze any WARN_ON messages to identify root cause

## References

- [Linux Kernel Debugging Guide](https://www.kernel.org/doc/html/latest/admin-guide/bug-hunting.html)
- [Android Kernel Debugging](https://source.android.com/docs/core/architecture/kernel/debugging)
- [Pstore Documentation](https://www.kernel.org/doc/html/latest/admin-guide/pstore-blk.html)
