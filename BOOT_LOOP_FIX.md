# Boot Loop Fix - Fstab Missing Issue

## Problem Analysis

The kernel log shows that the boot loop is caused by Android init failing to find fstab entries:

```
[    3.133174][    T1] init: [libfs_mgr] ReadFstabFromDt(): failed to read fstab from dt
[    3.156260][    T1] init: [libfs_mgr] ReadDefaultFstab(): failed to find device default fstab
[    3.160865][    T1] init: Failed to create FirstStageMount failed to read default fstab for first stage mount
[    3.165849][    T1] init: Failed to mount required partitions early ...
[    3.236423][    T1] Kernel panic - not syncing: Attempted to kill init! exitcode=0x00007f00
```

## Root Cause

This is **NOT a kernel issue** - it's a boot image/ramdisk configuration problem:

1. The test device is "linux,dummy-virt" (QEMU/emulator)
2. The init process in the ramdisk is looking for fstab entries
3. The fstab is missing from:
   - Device tree (`/firmware/android/fstab/*`)
   - Ramdisk (`/vendor/etc/fstab.*`, `/first_stage_ramdisk/fstab.*`, etc.)
4. Init gives up and exits with error code 127 (0x7f00)
5. Kernel panics because PID 1 (init) died

## Solution

### For Production Devices

Your boot image needs a proper fstab. Create a file in the ramdisk at one of these locations:
- `/vendor/etc/fstab.<device>`
- `/first_stage_ramdisk/fstab.<device>`
- Add fstab entries to device tree

Example minimal fstab for virtual/test devices:
```
# device          mount point  type  flags                      options
system           /system      ext4  ro,barrier=1               wait,slotselect,avb=vbmeta_system,logical,first_stage_mount
vendor           /vendor      ext4  ro,barrier=1               wait,slotselect,avb=vbmeta_system,logical,first_stage_mount
/dev/block/zram0 none         swap  defaults                   zramsize=50%
```

### For Emulator/Virtual Devices

If testing on QEMU/emulator:

1. **Use a proper Android system image** with complete ramdisk
2. **Or create a minimal boot image** with fstab:
   ```bash
   # Create ramdisk directory
   mkdir -p ramdisk/first_stage_ramdisk
   
   # Create minimal fstab
   cat > ramdisk/first_stage_ramdisk/fstab.dummy_virt << 'EOF'
   # Minimal fstab for dummy-virt
   /dev/block/vda  /system  ext4  ro  wait
   /dev/block/vdb  /vendor  ext4  ro  wait
   /dev/block/vdc  /data    ext4  noatime,nosuid,nodev,barrier=1  wait,check
   EOF
   
   # Pack ramdisk
   cd ramdisk
   find . | cpio -o -H newc | gzip > ../ramdisk.cpio.gz
   
   # Create boot image with your kernel + this ramdisk
   mkbootimg --kernel Image.gz --ramdisk ramdisk.cpio.gz --output boot.img
   ```

### Workaround for Development/Testing

If you just want to test the kernel without a full Android environment, you can:

1. **Modify kernel command line** to skip init and use a simpler init:
   ```
   init=/bin/sh
   ```

2. **Use a minimal init script** in the ramdisk that doesn't require fstab

## What This Repository Can't Fix

- The kernel cannot inject fstab entries that init will recognize
- Modifying device tree at runtime doesn't help (init reads flattened DT from bootloader)
- This is fundamentally a ramdisk/boot image issue, not a kernel compilation issue

## What We Can Do

We've added better documentation and logging to help diagnose this issue faster.

## Verify Your Boot Image

To check if your boot image has the necessary fstab:

```bash
# Extract boot image
unpack_bootimg --boot_img boot.img

# Extract ramdisk
gunzip -c ramdisk.cpio.gz | cpio -i

# Check for fstab
find . -name "fstab*"
```

If no fstab files are found, that's your problem.

## For This Repository's Users

If you're using the kernels built by this repository:

1. **Don't test on bare emulators** - use full Android virtual devices
2. **Ensure your device's boot partition** has the proper fstab before flashing the kernel
3. **Keep your original ramdisk** when repacking boot images with the new kernel
4. **Check device-specific requirements** - some devices need special fstab entries

## References

- [Android Init Documentation](https://source.android.com/docs/core/architecture/bootloader/system-as-root)
- [Fstab Format](https://source.android.com/docs/core/architecture/bootloader/partitions-images#fstab)
- [First Stage Mount](https://source.android.com/docs/core/architecture/partitions/early-mount)
