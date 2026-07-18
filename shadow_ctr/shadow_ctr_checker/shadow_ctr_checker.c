// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ctr_checker - runtime diagnostics for the shadow_ctr module family
 *
 * Registers a read-only character device /dev/shadow_ctr_checker. Reading it
 * (e.g. `cat /dev/shadow_ctr_checker`) produces a plain, greppable one-line-
 * per-check report describing, for each container-relevant kernel feature,
 * whether it is:
 *   - supported natively by this kernel build (compile-time IS_ENABLED(), since
 *     this module is compiled against the exact target kernel's config via the
 *     same DDK image as the other shadow modules), and/or
 *   - provided at runtime by a loaded shadow_* module.
 *
 * ---------------------------------------------------------------------------
 * Optional-dependency technique (important design decision)
 * ---------------------------------------------------------------------------
 * The whole point of this checker is to work standalone regardless of which
 * subset of the other shadow_* modules happens to be loaded. It therefore has
 * NO build-time (KBUILD_EXTRA_SYMBOLS / Module.symvers) dependency edge on any
 * other shadow module. If it did, insmod of the checker would fail with an
 * "unknown symbol" error whenever a queried module was not loaded.
 *
 * Instead it uses the kernel's standard runtime symbol-resolution primitive,
 * symbol_get()/symbol_put() (backed by __symbol_get()), uniformly for every
 * shadow module it queries:
 *   - each queryable shadow module exports a tiny presence marker
 *     (shadow_mqueue_is_active / shadow_sysvipc_is_active /
 *     shadow_cgdevices_is_active / shadow_overlay2_is_active, and
 *     shadow_ns_base's shadow_ns_base_type_loaded / _type_real);
 *   - symbol_get("...") returns non-NULL only if the providing module is
 *     currently loaded (and pins it for the duration, released via
 *     symbol_put()), so a missing module is handled gracefully as
 *     "not loaded".
 * This is why there is deliberately no Kbuild dependency arrow from the
 * checker to any other module.
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
#include <linux/stdarg.h>

/* SHADOW_NS_TYPE_* enum (compile-time only; no symbol dependency). */
#include "include/uapi/shadow_ns.h"

#define SHADOW_CTR_CHECKER_VERSION "2.0"
#define SHADOW_CHECKER_REPORT_MAX  4096

/*
 * Prototypes for the optionally-present exported symbols we resolve at runtime
 * via symbol_get(). These declarations exist only so symbol_get()'s
 * typeof(&x) has a type to work with; they create NO link-time relocation, so
 * the checker still builds and loads with none of these modules present.
 */
extern int shadow_mqueue_is_active(void);
extern int shadow_sysvipc_is_active(void);
extern int shadow_cgdevices_is_active(void);
extern int shadow_overlay2_is_active(void);
extern bool shadow_ns_base_type_loaded(u32 type);
extern bool shadow_ns_base_type_real(u32 type);

/* Is a shadow module exporting @sym currently loaded? (pins+releases it) */
#define shadow_checker_module_loaded(sym)			\
	({							\
		typeof(&sym) __p = symbol_get(sym);		\
		bool __loaded = (__p != NULL);			\
		if (__p)					\
			symbol_put(sym);			\
		__loaded;					\
	})

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

/*
 * Emit a "<label>: ..." line for a feature that is either builtin, provided by
 * a named shadow module, or unsupported.
 */
static void shadow_checker_feature(struct shadow_checker_buf *b,
				  const char *label, bool builtin,
				  bool shadow_loaded, const char *shadow_ko)
{
	if (builtin)
		shadow_checker_add(b, "%s: supported (builtin)\n", label);
	else if (shadow_loaded)
		shadow_checker_add(b, "%s: supported (%s)\n", label, shadow_ko);
	else
		shadow_checker_add(b, "%s: not supported\n", label);
}

/* Namespace line: builtin config + shadow_ns hijack state. */
static void shadow_checker_ns(struct shadow_checker_buf *b, const char *label,
			     bool builtin, bool (*type_loaded)(u32),
			     bool (*type_real)(u32), u32 type)
{
	bool hijacked = type_loaded && type_loaded(type);
	bool real = hijacked && type_real && type_real(type);

	if (builtin && hijacked)
		shadow_checker_add(b,
			"%s: supported (builtin; also shadow_ns %s)\n",
			label, real ? "real" : "bookkeeping");
	else if (builtin)
		shadow_checker_add(b, "%s: supported (builtin)\n", label);
	else if (hijacked)
		shadow_checker_add(b, "%s: supported (shadow_ns %s)\n",
			label, real ? "real" : "bookkeeping");
	else
		shadow_checker_add(b, "%s: not supported\n", label);
}

static void shadow_checker_build_report(struct shadow_checker_buf *b)
{
	struct file_system_type *ovl;
	bool ovl_registered, ovl_builtin, ovl_is_shadow;
	bool (*ns_type_loaded)(u32);
	bool (*ns_type_real)(u32);

	b->len = 0;

	shadow_checker_add(b,
		"shadow_ctr_checker v%s - shadow container-support status\n",
		SHADOW_CTR_CHECKER_VERSION);
	shadow_checker_add(b, "----------------------------------------\n");

	/* --- IPC-ish subsystems --------------------------------------- */
	shadow_checker_feature(b, "mqueue",
		IS_ENABLED(CONFIG_POSIX_MQUEUE),
		shadow_checker_module_loaded(shadow_mqueue_is_active),
		"shadow_mqueue.ko");

	shadow_checker_feature(b, "sysvipc",
		IS_ENABLED(CONFIG_SYSVIPC),
		shadow_checker_module_loaded(shadow_sysvipc_is_active),
		"shadow_sysvipc.ko");

	shadow_checker_feature(b, "cgroup_device",
		IS_ENABLED(CONFIG_CGROUP_DEVICE),
		shadow_checker_module_loaded(shadow_cgdevices_is_active),
		"shadow_cgdevices.ko");

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

	/* --- namespaces: resolve shadow_ns_base's query API at runtime - */
	ns_type_loaded = symbol_get(shadow_ns_base_type_loaded);
	ns_type_real = ns_type_loaded ? symbol_get(shadow_ns_base_type_real)
				      : NULL;

	shadow_checker_add(b,
		"# namespaces (task explicitly requests net/pid/ipc/uts; mnt/user/cgroup shown for completeness)\n");

	shadow_checker_ns(b, "ns_net", IS_ENABLED(CONFIG_NET_NS),
			 ns_type_loaded, ns_type_real, SHADOW_NS_TYPE_NET);
	shadow_checker_ns(b, "ns_pid", IS_ENABLED(CONFIG_PID_NS),
			 ns_type_loaded, ns_type_real, SHADOW_NS_TYPE_PID);
	shadow_checker_ns(b, "ns_ipc", IS_ENABLED(CONFIG_IPC_NS),
			 ns_type_loaded, ns_type_real, SHADOW_NS_TYPE_IPC);
	shadow_checker_ns(b, "ns_uts", IS_ENABLED(CONFIG_UTS_NS),
			 ns_type_loaded, ns_type_real, SHADOW_NS_TYPE_UTS);
	shadow_checker_ns(b, "ns_mnt", IS_ENABLED(CONFIG_MNT_NS),
			 ns_type_loaded, ns_type_real, SHADOW_NS_TYPE_MNT);
	/* User namespace: called out separately/explicitly per the task. */
	shadow_checker_ns(b, "ns_user (user namespace)",
			 IS_ENABLED(CONFIG_USER_NS),
			 ns_type_loaded, ns_type_real, SHADOW_NS_TYPE_USER);
	/*
	 * cgroup namespace has no dedicated Kconfig gate in mainline; CONFIG_
	 * CGROUPS is used as its proxy for the "builtin" column here.
	 */
	shadow_checker_ns(b, "ns_cgroup (proxy: CONFIG_CGROUPS)",
			 IS_ENABLED(CONFIG_CGROUPS),
			 ns_type_loaded, ns_type_real, SHADOW_NS_TYPE_CGROUP);

	if (ns_type_loaded)
		symbol_put(shadow_ns_base_type_loaded);
	if (ns_type_real)
		symbol_put(shadow_ns_base_type_real);
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
MODULE_DESCRIPTION("Runtime diagnostics for the shadow_ctr module family via /dev/shadow_ctr_checker (uses symbol_get for optional, standalone runtime detection of the other modules)");
MODULE_VERSION(SHADOW_CTR_CHECKER_VERSION);
