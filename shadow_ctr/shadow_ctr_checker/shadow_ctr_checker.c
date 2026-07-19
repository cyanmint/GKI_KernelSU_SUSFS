// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ctr_checker - runtime diagnostics for the shadow_ctr module family
 *
 * Registers a read-only character device /dev/shadow_ctr_checker. Reading it
 * (e.g. `cat /dev/shadow_ctr_checker`) produces a plain, greppable one-line-
 * per-check report describing, for each container-relevant kernel feature,
 * whether it is supported natively by this kernel build (compile-time
 * IS_ENABLED(), since this module is compiled against the exact target
 * kernel's config via the same DDK image as the other shadow modules), and/or
 * provided by a registered "overlay" filesystem type.
 *
 * ---------------------------------------------------------------------------
 * This module has NO dependency on any other shadow_ctr module
 * ---------------------------------------------------------------------------
 * shadow_ctr_checker is a pure, standalone diagnostics tool: it has no
 * build-time (KBUILD_EXTRA_SYMBOLS/Module.symvers) or load-time dependency on
 * shadow_ns, shadow_mqueue, shadow_sysvipc, shadow_cgdevices, shadow_hijack or
 * any other shadow_ctr module, and can be built and insmod'd entirely on its
 * own, in any order, regardless of which (if any) other shadow modules are
 * present.
 *
 * An earlier revision resolved shadow_mqueue_is_active()/_sysvipc_/
 * _cgdevices_is_active() and shadow_ns's query API purely at runtime via
 * symbol_get()/symbol_put() (backed by __symbol_get()/__symbol_put()).
 * However, __symbol_get()/__symbol_put() are trimmed from the exported-
 * symbol table of production GKI kernels (unreferenced by any built-in code,
 * so CONFIG_TRIM_UNUSED_KSYMS drops their EXPORT_SYMBOL entries even though
 * the functions themselves remain in the kernel image). Merely *referencing*
 * symbol_get()/symbol_put() anywhere in this module - even in a branch that
 * is never taken - makes the whole module fail to load with "Unknown symbol
 * __symbol_get"/"Unknown symbol __symbol_put" (insmod surfaces this as
 * -ENOENT, i.e. "No such file or directory"), since the kernel's module
 * loader must resolve every referenced symbol before the module can be
 * loaded at all, regardless of runtime control flow.
 *
 * A later revision instead took a normal build+load-time dependency on
 * shadow_ns's EXPORT_SYMBOL_GPL query API via KBUILD_EXTRA_SYMBOLS. That has
 * been removed too: this checker is meant to work standalone with zero
 * dependencies, so it no longer calls into shadow_ns at all. Since
 * shadow_ns.c computes whether it simulates (and whether that simulation is
 * functionally real vs. bookkeeping-only) a given namespace type using the
 * exact same IS_ENABLED(CONFIG_{UTS,IPC,USER,PID,NET}_NS) checks this module
 * already performs (both are built against the identical target kernel
 * config in the same DDK image), that same information can be derived here
 * directly at compile time without calling into shadow_ns at all - so the
 * "is it builtin" line already implies "if not builtin, shadow_ns.ko (if
 * loaded) provides real/bookkeeping isolation for it"; see
 * shadow_checker_ns() below and shadow_ctr/shadow_ns/README.md for which
 * types get real vs. bookkeeping-only simulation.
 *
 * The shadow_mqueue/shadow_sysvipc/shadow_cgdevices "is this specific .ko
 * providing it" distinction has also been dropped (there is no safe way left
 * to query optional, independently-loadable modules without symbol_get());
 * this checker now only reports whether the underlying kernel feature is
 * built in. Use `cat /proc/modules` (or `lsmod`) to see which shadow_*
 * modules are actually loaded.
 *
 * For overlayfs specifically the checker additionally consults
 * get_fs_type("overlay") as ground truth, because that (not IS_ENABLED alone)
 * is what determines whether mount(2) of an overlay will actually work: overlay
 * could be builtin (=y), a genuine loadable overlay.ko, or provided by
 * shadow_overlay2.ko, and get_fs_type() is the authoritative signal.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/minmax.h>
/*
 * <linux/stdarg.h> (a thin wrapper the kernel ships so in-tree code doesn't
 * pull in the compiler's freestanding <stdarg.h> directly) was only added in
 * v5.15 (commit c0891ac15f0428ffa81b2e818d416bdf3cb74ab6, "isystem: ship and
 * use stdarg.h"). It doesn't exist on the android12-5.10/android13-5.10 GKI
 * kernels this module also targets, so fall back to the compiler-provided
 * <stdarg.h> there.
 */
#if __has_include(<linux/stdarg.h>)
#include <linux/stdarg.h>
#else
#include <stdarg.h>
#endif

#define SHADOW_CTR_CHECKER_VERSION "2.0"
#define SHADOW_CHECKER_REPORT_MAX  4096

struct shadow_checker_buf {
	char	data[SHADOW_CHECKER_REPORT_MAX];
	size_t	len;
};

static __printf(2, 3) void
shadow_checker_add(struct shadow_checker_buf *b, const char *fmt, ...)
{
	va_list args;
	int n;

	if (b->len >= sizeof(b->data))
		return;
	va_start(args, fmt);
	n = vsnprintf(b->data + b->len, sizeof(b->data) - b->len, fmt, args);
	va_end(args);
	if (n > 0)
		b->len += min_t(size_t, (size_t)n, sizeof(b->data) - b->len - 1);
}

/* Emit a "<label>: ..." line for a feature that is either builtin or not. */
static void shadow_checker_feature(struct shadow_checker_buf *b,
				  const char *label, bool builtin)
{
	if (builtin)
		shadow_checker_add(b, "%s: supported (builtin)\n", label);
	else
		shadow_checker_add(b,
			"%s: not supported (not builtin); check `lsmod`/`/proc/modules` for a shadow_* provider\n",
			label);
}

/* Namespace type: whether shadow_ns's fallback simulation (if loaded) for
 * this type performs genuine functional isolation or bookkeeping only. See
 * shadow_ctr/shadow_ns/README.md.
 */
enum shadow_checker_ns_sim {
	SHADOW_CHECKER_NS_SIM_NONE,		/* always builtin; no fallback needed */
	SHADOW_CHECKER_NS_SIM_REAL,		/* shadow_ns provides real isolation */
	SHADOW_CHECKER_NS_SIM_BOOKKEEPING,	/* shadow_ns provides bookkeeping only */
};

/*
 * Namespace line: builtin config, plus (if not builtin) what shadow_ns.ko -
 * if loaded - would provide for it. This module has no build/load-time
 * dependency on shadow_ns, so it cannot query whether shadow_ns.ko is
 * actually loaded right now; it only states what it *would* provide, since
 * that is fully determined at compile time by the same IS_ENABLED(CONFIG_*)
 * checks shadow_ns.c itself uses.
 */
static void shadow_checker_ns(struct shadow_checker_buf *b, const char *label,
			     bool builtin, enum shadow_checker_ns_sim sim)
{
	if (builtin) {
		shadow_checker_add(b, "%s: supported (builtin)\n", label);
		return;
	}
	switch (sim) {
	case SHADOW_CHECKER_NS_SIM_REAL:
		shadow_checker_add(b,
			"%s: not supported (not builtin); shadow_ns.ko provides real isolation if loaded - check lsmod\n",
			label);
		break;
	case SHADOW_CHECKER_NS_SIM_BOOKKEEPING:
		shadow_checker_add(b,
			"%s: not supported (not builtin); shadow_ns.ko provides bookkeeping-only fallback if loaded - check lsmod\n",
			label);
		break;
	default:
		shadow_checker_add(b, "%s: not supported\n", label);
		break;
	}
}

static void shadow_checker_build_report(struct shadow_checker_buf *b)
{
	struct file_system_type *ovl;
	bool ovl_registered, ovl_builtin, ovl_is_shadow;

	b->len = 0;

	shadow_checker_add(b,
		"shadow_ctr_checker v%s - shadow container-support status\n",
		SHADOW_CTR_CHECKER_VERSION);
	shadow_checker_add(b, "----------------------------------------\n");

	/* --- IPC-ish subsystems --------------------------------------- */
	shadow_checker_feature(b, "mqueue", IS_ENABLED(CONFIG_POSIX_MQUEUE));
	shadow_checker_feature(b, "sysvipc", IS_ENABLED(CONFIG_SYSVIPC));
	shadow_checker_feature(b, "cgroup_device", IS_ENABLED(CONFIG_CGROUP_DEVICE));

	/* --- overlayfs: get_fs_type() is ground truth ----------------- */
	ovl = get_fs_type("overlay");
	ovl_registered = (ovl != NULL);
	ovl_builtin = ovl_registered && (ovl->owner == NULL);
	ovl_is_shadow = ovl_registered && ovl->owner &&
			!strcmp(ovl->owner->name, "shadow_overlay2");
	if (ovl)
		module_put(ovl->owner); /* balance get_fs_type()'s ref; NULL-safe */

	if (!ovl_registered)
		shadow_checker_add(b, "overlay2: not supported\n");
	else if (ovl_builtin)
		shadow_checker_add(b, "overlay2: supported (builtin)\n");
	else if (ovl_is_shadow)
		shadow_checker_add(b,
			"overlay2: supported (shadow_overlay2.ko)\n");
	else
		shadow_checker_add(b,
			"overlay2: supported (module: %s)\n", ovl->owner->name);

	/* --- namespaces: builtin config; shadow_ns.ko is optional -------- */
	shadow_checker_add(b,
		"# namespaces (task explicitly requests net/pid/ipc/uts; mnt/user/cgroup shown for completeness)\n");

	shadow_checker_ns(b, "ns_net", IS_ENABLED(CONFIG_NET_NS),
			 SHADOW_CHECKER_NS_SIM_BOOKKEEPING);
	shadow_checker_ns(b, "ns_pid", IS_ENABLED(CONFIG_PID_NS),
			 SHADOW_CHECKER_NS_SIM_REAL);
	shadow_checker_ns(b, "ns_ipc", IS_ENABLED(CONFIG_IPC_NS),
			 SHADOW_CHECKER_NS_SIM_BOOKKEEPING);
	shadow_checker_ns(b, "ns_uts", IS_ENABLED(CONFIG_UTS_NS),
			 SHADOW_CHECKER_NS_SIM_REAL);
	shadow_checker_ns(b, "ns_mnt", IS_ENABLED(CONFIG_MNT_NS),
			 SHADOW_CHECKER_NS_SIM_NONE);
	/* User namespace: called out separately/explicitly per the task. */
	shadow_checker_ns(b, "ns_user (user namespace)",
			 IS_ENABLED(CONFIG_USER_NS), SHADOW_CHECKER_NS_SIM_REAL);
	/*
	 * cgroup namespace has no dedicated Kconfig gate in mainline; CONFIG_
	 * CGROUPS is used as its proxy for the "builtin" column here.
	 */
	shadow_checker_ns(b, "ns_cgroup (proxy: CONFIG_CGROUPS)",
			 IS_ENABLED(CONFIG_CGROUPS), SHADOW_CHECKER_NS_SIM_NONE);
}

static int shadow_checker_open(struct inode *inode, struct file *file)
{
	struct shadow_checker_buf *b;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	/* Snapshot the report fresh on every open. */
	shadow_checker_build_report(b);
	file->private_data = b;
	return 0;
}

static int shadow_checker_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t shadow_checker_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct shadow_checker_buf *b = file->private_data;

	return simple_read_from_buffer(buf, count, ppos, b->data, b->len);
}

static const struct file_operations shadow_checker_fops = {
	.owner		= THIS_MODULE,
	.open		= shadow_checker_open,
	.read		= shadow_checker_read,
	.release	= shadow_checker_release,
	.llseek		= default_llseek,
};

static struct miscdevice shadow_checker_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "shadow_ctr_checker",
	.fops	= &shadow_checker_fops,
	.mode	= 0444,
};

static int __init shadow_ctr_checker_init(void)
{
	int ret;

	ret = misc_register(&shadow_checker_misc);
	if (ret) {
		pr_err("shadow_ctr_checker: misc_register() failed: %d\n", ret);
		return ret;
	}

	pr_info("shadow_ctr_checker: loaded (version %s); read /dev/shadow_ctr_checker for status\n",
		SHADOW_CTR_CHECKER_VERSION);
	return 0;
}

static void __exit shadow_ctr_checker_exit(void)
{
	misc_deregister(&shadow_checker_misc);
	pr_info("shadow_ctr_checker: unloaded\n");
}

module_init(shadow_ctr_checker_init);
module_exit(shadow_ctr_checker_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Runtime diagnostics for the shadow_ctr module family via /dev/shadow_ctr_checker (standalone; no dependency on any other shadow_ctr module)");
MODULE_VERSION(SHADOW_CTR_CHECKER_VERSION);
