# Init Exit Diagnostics Patch

## Purpose

This patch adds detailed diagnostic information when Android's init process (PID 1) exits, which typically causes a kernel panic and boot loop.

## What it Does

When init exits (usually due to configuration errors), instead of just showing:
```
Kernel panic - not syncing: Attempted to kill init! exitcode=0x00007f00
```

The kernel will now show:
```
====================================================
INIT PROCESS EXITED
====================================================
Init (PID 1) exited with code: 0x00007f00
Exit code breakdown:
  Signal: 127 (0x7f)
  Exit status: 0

Common causes:
1. Missing fstab (check device tree and ramdisk)
2. Corrupted or incompatible init binary
3. Missing system partitions
4. SELinux policy issues
5. Ramdisk/kernel version mismatch

Check kernel log above for init error messages.
For fstab issues, see BOOT_LOOP_FIX.md
...
====================================================

Kernel panic - not syncing: Attempted to kill init! exitcode=0x00007f00
```

## Benefits

1. **Faster diagnosis**: Immediately see common causes without searching documentation
2. **Better error messages**: Exit code is broken down into signal and status
3. **Actionable guidance**: Points to specific things to check
4. **Reduced debugging time**: No need to search for "what does exit code 0x7f00 mean?"

## When to Use

- **Development/Testing**: Always useful to have better error messages
- **Production**: Safe to include - only triggers when init exits (which is always fatal anyway)
- **CI/CD**: Helps automated systems report more useful error information

## Patch Files

- `a12-5.10/kernel/exit.c.debug.patch` - Android 12 (Kernel 5.10)
- `a13-5.15/kernel/exit.c.debug.patch` - Android 13 (Kernel 5.15)
- `a14-6.1/kernel/exit.c.debug.patch` - Android 14 (Kernel 6.1)
- `a15-6.6/kernel/exit.c.debug.patch` - Android 15 (Kernel 6.6)
- `a16-6.12/kernel/exit.c.debug.patch` - Android 16 (Kernel 6.12)

## How to Apply

### Automatic (via build system)

The build system will automatically apply this patch when `--containerd` flag is used.

### Manual

```bash
cd kernel_source/common
patch -p1 < path/to/exit.c.debug.patch
```

## Configuration

This patch is automatically applied when building with containerd support. To apply it manually in other scenarios, edit the kernel builder script:

```python
# In kernel_builder.py, add to the patch application logic:
if self.config.debug_mode:  # or always apply
    debug_patch = patch_dir / "kernel/exit.c.debug.patch"
    if debug_patch.exists():
        self._run_cmd(f"patch -p1 < {debug_patch}")
```

## Overhead

- **Code size**: ~2KB additional code in kernel image
- **Runtime**: No overhead in normal operation
- **Performance**: Only executes when init exits (always fatal, so performance doesn't matter)

## Safety

✅ **Safe for production**: This patch only adds diagnostic logging. It doesn't change behavior.

✅ **No functional changes**: The kernel still panics exactly as before, just with better messages.

✅ **No security impact**: Only adds pr_err() calls with static diagnostic text.

## Related Documentation

- `BOOT_LOOP_FIX.md` - Comprehensive guide to fixing boot loops
- `README.md` - Main containerd patches documentation
- `DEBUGGING_GUIDE.md` - Kernel debugging configuration

## Example Output

Here's an example of what you'll see in the kernel log when init exits due to missing fstab:

```
[    3.133174][    T1] init: [libfs_mgr] ReadFstabFromDt(): failed to read fstab from dt
[    3.156260][    T1] init: [libfs_mgr] ReadDefaultFstab(): failed to find device default fstab
[    3.160865][    T1] init: Failed to create FirstStageMount failed to read default fstab
[    3.165849][    T1] init: Failed to mount required partitions early ...
[    3.177345][    T1] ====================================================
[    3.177345][    T1] INIT PROCESS EXITED
[    3.177345][    T1] ====================================================
[    3.177345][    T1] Init (PID 1) exited with code: 0x00007f00
[    3.177345][    T1] Exit code breakdown:
[    3.177345][    T1]   Signal: 127 (0x7f)
[    3.177345][    T1]   Exit status: 0
[    3.177345][    T1] 
[    3.177345][    T1] Common causes:
[    3.177345][    T1] 1. Missing fstab (check device tree and ramdisk)
[    3.177345][    T1] 2. Corrupted or incompatible init binary
[    3.177345][    T1] 3. Missing system partitions
[    3.177345][    T1] 4. SELinux policy issues
[    3.177345][    T1] 5. Ramdisk/kernel version mismatch
[    3.177345][    T1] 
[    3.177345][    T1] Check kernel log above for init error messages.
[    3.177345][    T1] For fstab issues, see BOOT_LOOP_FIX.md
[    3.177345][    T1] 
[    3.177345][    T1] If testing on emulator/virtual device:
[    3.177345][    T1]   - Ensure proper fstab in ramdisk or device tree
[    3.177345][    T1]   - Use complete Android system image, not bare kernel
[    3.177345][    T1]   - Check that device tree is compatible with init
[    3.177345][    T1] ====================================================
[    3.177345][    T1] 
[    3.177345][    T1] System will now panic and reboot...
[    3.177345][    T1] Kernel panic - not syncing: Attempted to kill init! exitcode=0x00007f00
```

## Troubleshooting

**Q: The patch won't apply**  
A: Check that you're applying it to the correct kernel version. The line numbers may differ slightly between kernel versions, but the structure of `do_exit()` is generally stable.

**Q: I don't see the diagnostic messages**  
A: Make sure your kernel log level is high enough. Use `loglevel=8` on kernel command line.

**Q: Can I customize the diagnostic messages?**  
A: Yes, edit the patch file to modify the pr_err() messages before applying.

## Credits

Created to help diagnose boot loops caused by missing fstab configuration in containerized Android environments.
