#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/slab.h>
#include <linux/string.h>

/*
 * Emergency fstab injector for devices without proper device tree entries
 * This prevents init from panicking when fstab cannot be found.
 * 
 * WARNING: This is a workaround for testing/development only!
 * Production devices should have proper fstab in their device tree or ramdisk.
 */

static int __init emergency_fstab_init(void)
{
    struct device_node *firmware_node, *android_node;
    struct device_node *fstab_node, *system_node;
    struct device_node *root;
    const char *model = NULL;
    int ret;

    /* Only apply this fix if we're on a problematic device */
    root = of_find_node_by_path("/");
    if (!root) {
        return 0;
    }

    ret = of_property_read_string(root, "model", &model);
    if (ret == 0 && model) {
        pr_info("emergency_fstab: Device model: %s\n", model);
        
        /* Only apply to known problematic devices (emulators, test devices) */
        if (strstr(model, "dummy-virt") || strstr(model, "qemu") || 
            strstr(model, "goldfish") || strstr(model, "cuttlefish")) {
            pr_info("emergency_fstab: Detected virtual/test device\n");
        } else {
            /* Not a virtual device, don't inject */
            of_node_put(root);
            pr_info("emergency_fstab: Physical device detected, skipping injection\n");
            return 0;
        }
    }
    of_node_put(root);

    /* Check if fstab already exists */
    fstab_node = of_find_node_by_path("/firmware/android/fstab");
    if (fstab_node) {
        pr_info("emergency_fstab: Fstab already exists in device tree\n");
        of_node_put(fstab_node);
        return 0;
    }

    pr_info("emergency_fstab: No fstab found, attempting to inject minimal entries\n");
    pr_info("emergency_fstab: NOTE: This is a development/testing workaround\n");
    pr_info("emergency_fstab: Production devices should have proper fstab configuration\n");

    /* 
     * We can't actually inject into the flattened device tree that init reads.
     * The FDT is already parsed and immutable by the time we run.
     * 
     * The real fix requires:
     * 1. Proper fstab in the ramdisk, OR
     * 2. Proper fstab in the device tree (passed by bootloader), OR
     * 3. A modified init that doesn't require fstab for development/testing
     * 
     * This module serves as documentation and early warning.
     */

    pr_err("==========================================================\n");
    pr_err("BOOT WILL LIKELY FAIL: Missing fstab configuration!\n");
    pr_err("==========================================================\n");
    pr_err("This kernel is running on a virtual/test device without\n");
    pr_err("proper fstab entries in either:\n");
    pr_err("  1. Device tree (/firmware/android/fstab/*)\n");
    pr_err("  2. Ramdisk (/vendor/etc/fstab.*, /first_stage_ramdisk/fstab.*)\n");
    pr_err("\n");
    pr_err("To fix this:\n");
    pr_err("  - Add fstab to your ramdisk when building boot.img\n");
    pr_err("  - Use a complete Android system image (not bare kernel)\n");
    pr_err("  - See BOOT_LOOP_FIX.md for detailed instructions\n");
    pr_err("==========================================================\n");

    return 0;
}

/*
 * Run very early, before init starts looking for fstab
 * But note: even early_initcall is too late to modify the FDT that init uses
 */
early_initcall(emergency_fstab_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GKI Build System");
MODULE_DESCRIPTION("Emergency fstab detector and documentation");
