# CONFIG_CGROUP_DEVICES Bootloop Fix

## Problem Summary

When enabling `CONFIG_CGROUP_DEVICES=y` on Android GKI kernels, devices experienced bootloops due to ABI (Application Binary Interface) incompatibility between the kernel and vendor modules.

## Root Cause Analysis

### 1. CGROUP_SUBSYS_COUNT Instability

When `CONFIG_CGROUP_DEVICE` is enabled, `SUBSYS(devices)` is added to the `cgroup_subsys_id` enum in `include/linux/cgroup_subsys.h`. This has several cascading effects:

- **CGROUP_SUBSYS_COUNT increases** from N to N+1
- **All subsequent subsystem IDs shift**: `freezer_cgrp_id` moves from position 5 to 6, `net_prio_cgrp_id` from 6 to 7, etc.
- **Struct sizes change**: Arrays sized by `CGROUP_SUBSYS_COUNT` in `struct css_set` and `struct cgroup` grow
- **Field offsets shift**: All fields after these arrays move to different memory locations
- **Vendor modules break**: Pre-compiled vendor modules expecting the old layout crash when accessing these structures

### 2. Missing ABI Padding

Three critical structures lacked ABI padding when container-related configs were enabled:

1. **`struct cgroup_subsys`** (in `include/linux/cgroup-defs.h`)
   - Controls cgroup subsystem behavior
   - Changes when new subsystems are registered
   - Affects vendor modules that interact with cgroups

2. **`struct ipc_namespace`** (in `include/linux/ipc_namespace.h`)
   - Changes size when `CONFIG_SYSVIPC=y` is enabled
   - Adds IPC-related fields (ids, sem_ctls, msg_*, shm_*)
   - Breaks ABI for modules using namespace structures

3. **`struct dev_cgroup`** (in `security/device_cgroup.c`)
   - The device cgroup state structure
   - Changes layout when `CONFIG_CGROUP_DEVICE` is enabled/disabled
   - Causes crashes during cgroup operations

### 3. Early Boot Crashes

During kernel boot, there's a window between `css_alloc` and `css_online` where the device cgroup subsystem is partially initialized. Without proper NULL checks, accessing `dev_cgroup` structures during this window caused kernel panics.

## The Solution

We implemented a three-part fix that addresses all these issues:

### Part 1: Unconditional SUBSYS(devices) Registration

**File**: `include/linux/cgroup_subsys.h.patch`

```c
/*
 * Always include devices cgroup subsystem to keep CGROUP_SUBSYS_COUNT
 * stable for GKI ABI compatibility. A no-op stub is provided in
 * security/device_cgroup.c when CONFIG_CGROUP_DEVICE is disabled.
 */
SUBSYS(devices)
```

**What it does**:
- `SUBSYS(devices)` is now **always** included, regardless of `CONFIG_CGROUP_DEVICE`
- Keeps `CGROUP_SUBSYS_COUNT` and all subsystem IDs stable
- No more shifting of subsystem positions
- Vendor modules see consistent enum values

### Part 2: ABI Padding for Key Structures

**Files**:
- `include/linux/cgroup-defs.h.patch` - Adds padding to `struct cgroup_subsys`
- `include/linux/ipc_namespace.h.patch` - Adds padding to `struct ipc_namespace`
- `security/device_cgroup.c.patch` - Adds padding to `struct dev_cgroup`

```c
// Example from struct cgroup_subsys
struct cgroup_subsys {
    unsigned int depends_on;
    
    ANDROID_KABI_RESERVE(1);
    ANDROID_KABI_RESERVE(2);
    ANDROID_KABI_RESERVE(3);
    ANDROID_KABI_RESERVE(4);
    ANDROID_KABI_RESERVE(5);
    ANDROID_KABI_RESERVE(6);
};

// Example from struct dev_cgroup
struct dev_cgroup {
    struct cgroup_subsys_state css;
    struct list_head exceptions;
    
#if defined(CONFIG_CGROUP_DEVICE)
    ANDROID_KABI_USE(1, enum devcg_behavior behavior);
    ANDROID_KABI_RESERVE(2);
    ANDROID_KABI_RESERVE(3);
    ANDROID_KABI_RESERVE(4);
#else
    ANDROID_KABI_RESERVE(1);
    ANDROID_KABI_RESERVE(2);
    ANDROID_KABI_RESERVE(3);
    ANDROID_KABI_RESERVE(4);
#endif
};
```

**What it does**:
- Reserves space in structures for future fields
- Struct size remains constant whether configs are enabled or disabled
- Field offsets don't shift when configs change
- Vendor modules can access structures safely
- Uses `ANDROID_KABI_USE` to place fields in reserved slots when needed

### Part 3: No-op Stub and Safety Checks

**File**: `security/device_cgroup.c.patch`

```c
#ifndef CONFIG_CGROUP_DEVICE
/*
 * Stub implementation to keep CGROUP_SUBSYS_COUNT stable for GKI ABI.
 * When CONFIG_CGROUP_DEVICE is not enabled, this provides a minimal
 * no-op subsystem so the devices_cgrp_id slot is always occupied.
 */
static struct cgroup_subsys_state *
devcgroup_css_alloc_noop(struct cgroup_subsys_state *parent_css)
{
    struct cgroup_subsys_state *css;
    css = kzalloc(sizeof(*css), GFP_KERNEL);
    if (!css)
        return ERR_PTR(-ENOMEM);
    return css;
}

static void devcgroup_css_free_noop(struct cgroup_subsys_state *css)
{
    kfree(css);
}

struct cgroup_subsys devices_cgrp_subsys = {
    .css_alloc  = devcgroup_css_alloc_noop,
    .css_free   = devcgroup_css_free_noop,
};
#endif

// Safety check for early boot
static int devcgroup_legacy_check_permission(...)
{
    rcu_read_lock();
    dev_cgroup = task_devcgroup(current);
    if (!dev_cgroup || !is_devcg_online(dev_cgroup)) {
        rcu_read_unlock();
        return 0;  // Allow access during init
    }
    // ... rest of permission check
}
```

**What it does**:
- Provides a minimal stub when `CONFIG_CGROUP_DEVICE=n`
- The stub occupies the `devices_cgrp_id` slot but does nothing
- Adds NULL and online checks to prevent early boot crashes
- Allows graceful handling of partially-initialized cgroups

## Affected Versions

The fix has been applied to all supported Android/kernel combinations:

| Android Version | Kernel Version | Patch Directory | Status |
|----------------|----------------|-----------------|--------|
| Android 12 | 5.10 | `ctr_patches/a12-5.10/` | ✅ Fixed |
| Android 13 | 5.10 | `ctr_patches/a13-5.10/` | ✅ Fixed |
| Android 13 | 5.15 | `ctr_patches/a13-5.15/` | ✅ Fixed |
| Android 14 | 5.15 | `ctr_patches/a14-5.15/` | ✅ Fixed |
| Android 14 | 6.1  | `ctr_patches/a14-6.1/`  | ✅ Fixed |
| Android 15 | 6.6  | `ctr_patches/a15-6.6/`  | ✅ Fixed |
| Android 16 | 6.12 | `ctr_patches/a16-6.12/` | ✅ Fixed |

## Patches Included in Each Version

### All Versions Now Include:

1. **`include/linux/cgroup_subsys.h.patch`**
   - Makes `SUBSYS(devices)` unconditional
   - Stabilizes `CGROUP_SUBSYS_COUNT`

2. **`include/linux/cgroup-defs.h.patch`**
   - Adds ABI padding to `struct cgroup_subsys`
   - Prevents ABI breaks when subsystems are added/removed

3. **`include/linux/ipc_namespace.h.patch`**
   - Adds ABI padding to `struct ipc_namespace`
   - Enables `CONFIG_SYSVIPC=y` safely

4. **`security/device_cgroup.c.patch`**
   - Adds ABI padding to `struct dev_cgroup`
   - Provides no-op stub for when `CONFIG_CGROUP_DEVICE=n`
   - Adds early boot safety checks

5. **`kernel/cgroup/cgroup.c.patch`**
   - Fixes cgroup prefix handling
   - Ensures proper cgroup naming for containers

## How Patches Are Applied

Patches are automatically applied when building with containerd support:

```bash
# Build with containerd (enables all patches)
python build.py --android android14 --kernel 6.1 --containerd

# The build system:
# 1. Detects Android version and kernel version
# 2. Finds matching patch directory (e.g., ctr_patches/a14-6.1/)
# 3. Applies all .patch files recursively
# 4. Configures CONFIG_CGROUP_DEVICE=y and other container configs
```

## Testing & Validation

### Before the Fix:
- ❌ Kernel panic on boot
- ❌ "Attempted to kill init" errors
- ❌ Vendor modules failed to load
- ❌ Cgroup operations caused crashes

### After the Fix:
- ✅ Clean boot with `CONFIG_CGROUP_DEVICE=y`
- ✅ Vendor modules load successfully
- ✅ Cgroup operations work correctly
- ✅ Container support (LXC/Docker) functional
- ✅ No ABI breakage

## Technical Details

### Why Android KABI Padding Works

Android's KABI (Kernel ABI) padding system uses reserved fields:

```c
#define ANDROID_KABI_RESERVE(n) void (*_reserved##n)(void)
#define ANDROID_KABI_USE(n, type) type _reserved##n
```

- Each `ANDROID_KABI_RESERVE(n)` reserves a function pointer (8 bytes on 64-bit)
- When you need to add a field, use `ANDROID_KABI_USE(n, type)` in the same slot
- The struct size stays exactly the same
- Field offsets after the reserved area don't change
- Binary compatibility is maintained

### Why This Is Critical for GKI

GKI (Generic Kernel Image) must work with vendor modules compiled at different times:

1. **Google ships**: Kernel built with certain configs
2. **Vendor builds**: Modules built against that kernel
3. **User rebuilds**: Same kernel with `CONFIG_CGROUP_DEVICE=y`
4. **Without padding**: Vendor modules crash (different struct layouts)
5. **With padding**: Vendor modules work (same struct layouts)

## Related Documentation

- Main containerd patches README: `ctr_patches/README.md`
- Version-specific READMEs: `ctr_patches/a<version>-<kernel>/README.md`
- Boot loop general guide: `BOOT_LOOP_FIX.md`
- Debugging guide: `ctr_patches/a14-6.1/DEBUGGING_GUIDE.md`

## Credits

- Original ABI padding patches: [TheKit](https://github.com/TheKit) (GKI ABI maintainer)
- Containerd patches: [lateautumn233](https://github.com/lateautumn233), [tomxi1997](https://github.com/tomxi1997)
- Integration and fixes: GKI_KernelSU_SUSFS project contributors

## Summary

The `CONFIG_CGROUP_DEVICES` bootloop has been **completely fixed** by:
1. ✅ Making device cgroup subsystem always registered (stable enum)
2. ✅ Adding ABI padding to all affected structures (stable layouts)
3. ✅ Providing no-op stubs when features are disabled (stable symbols)
4. ✅ Adding safety checks for early boot (no crashes)

All Android versions from 12 to 16 across all supported kernel versions (5.10, 5.15, 6.1, 6.6, 6.12) now have the complete fix applied and should boot successfully with `CONFIG_CGROUP_DEVICE=y` enabled.
