// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_configspoof - kernel config spoofing overlay for /proc/config.gz
 *
 * A standalone loadable kernel module that overlays /proc/config.gz with a
 * pre-built, gzip-compressed configuration text advertising the CONFIG_*
 * symbols container runtimes probe for (CONFIG_NAMESPACES, CONFIG_SYSVIPC,
 * CONFIG_POSIX_MQUEUE, CONFIG_CGROUP_DEVICE, netfilter/bridge symbols, ...)
 * as "y", independent of what vmlinux was actually built with.
 *
 * Motivation
 * ----------
 * dockerd's contrib/check-config.sh, containerd's CRI feature probing, and
 * various "docker info" preflight warnings parse /proc/config.gz (or
 * /boot/config-$(uname -r)) and refuse to start, or print scary warnings and
 * disable features, when expected CONFIG_* symbols are missing. On a GKI
 * kernel built without several of those options (because they cannot be
 * enabled without breaking GKI KMI compatibility, or simply were not turned
 * on), those preflight checks fail even though the shadow_* modules
 * (shadow_ns, shadow_sysvipc, shadow_mqueue, shadow_cgdevices) already
 * provide (partial, honestly-documented) behaviour for the corresponding
 * subsystem via transparent syscall hijacking.
 *
 * shadow_configspoof closes that last gap: it makes the *self-report* the
 * running kernel gives to userspace consistent with what the shadow modules
 * actually provide, so unconditional CONFIG_* preflight checks stop being a
 * hard blocker.
 *
 * IMPORTANT: this module only spoofs the *advertisement*. It does not, by
 * itself, change any kernel behaviour. Only load it together with the
 * shadow_* module(s) that back the CONFIG_* symbols you are spoofing -
 * spoofing CONFIG_SYSVIPC=y without shadow_sysvipc loaded just turns a clear
 * "-ENOSYS, container refused to start" failure into a confusing runtime
 * malfunction once the container actually calls msgget(2) and friends.
 *
 * Implementation
 * --------------
 * The exact gzip byte stream is generated at build time (see
 * gen_config_gz.py / config.template) rather than compressed at module-load
 * time, so the module has no dependency on CONFIG_ZLIB_DEFLATE being enabled
 * in the target kernel. At init, the module removes any pre-existing
 * /proc/config.gz entry (the in-tree CONFIG_IKCONFIG_PROC implementation, if
 * present) and installs its own read-only proc_ops that just hands the
 * embedded buffer to userspace via simple_read_from_buffer(); at exit it
 * removes its entry. It intentionally does NOT try to restore the original
 * ikconfig proc entry on unload (that would require capturing its internal
 * ikconfig_file_ops-derived proc_dir_entry beforehand and re-registering it,
 * which is not possible from a module) - unloading shadow_configspoof simply
 * leaves /proc/config.gz absent, matching the many GKI configs that already
 * ship CONFIG_IKCONFIG_PROC disabled.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#include "config_gz_data.h"
#include "shadow_ctr_internal.h"

static ssize_t shadow_configspoof_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	return simple_read_from_buffer(buf, count, ppos, shadow_configspoof_gz,
					shadow_configspoof_gz_len);
}

static const struct proc_ops shadow_configspoof_proc_ops = {
	.proc_read	= shadow_configspoof_read,
	.proc_lseek	= default_llseek,
};

static struct proc_dir_entry *shadow_configspoof_entry;

int __init shadow_configspoof_init(void)
{
	/*
	 * Try to create /proc/config.gz first. On most kernels nothing is
	 * there yet and this just succeeds outright -- no removal needed.
	 * proc_create() only returns NULL on a genuine name collision (e.g.
	 * CONFIG_IKCONFIG_PROC already registered the real /proc/config.gz),
	 * so we only fall back to remove_proc_entry() in that case, which
	 * avoids the harmless-but-alarming WARN() splat that
	 * remove_proc_entry() emits when asked to remove an entry that was
	 * never there in the first place.
	 */
	pr_info("shadow_configspoof: init: creating /proc/config.gz (%u bytes payload)\n",
		shadow_configspoof_gz_len);
	shadow_configspoof_entry = proc_create("config.gz", 0444, NULL,
					    &shadow_configspoof_proc_ops);
	if (!shadow_configspoof_entry) {
		pr_info("shadow_configspoof: init: /proc/config.gz already exists, removing and retrying\n");
		remove_proc_entry("config.gz", NULL);

		shadow_configspoof_entry = proc_create("config.gz", 0444, NULL,
						    &shadow_configspoof_proc_ops);
	}
	if (!shadow_configspoof_entry) {
		pr_err("shadow_configspoof: failed to create /proc/config.gz\n");
		return -ENOMEM;
	}

	pr_info("shadow_configspoof: /proc/config.gz overlay installed (%u bytes)\n",
		shadow_configspoof_gz_len);
	return 0;
}

void shadow_configspoof_exit(void)
{
	if (shadow_configspoof_entry) {
		pr_info("shadow_configspoof: exit: removing /proc/config.gz\n");
		remove_proc_entry("config.gz", NULL);
		shadow_configspoof_entry = NULL;
	}
	pr_info("shadow_configspoof: /proc/config.gz overlay removed\n");
}
