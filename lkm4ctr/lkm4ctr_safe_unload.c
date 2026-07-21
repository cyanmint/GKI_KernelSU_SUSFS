// SPDX-License-Identifier: GPL-2.0
/*
 * lkm4ctr_safe_unload - sysfs-triggered self-unload for lkm4ctr.ko.
 *
 * Background
 * ----------
 * Every shadow_hook redirect point now brackets the time it spends inside
 * hook->function with a module reference (try_module_get()/module_put(),
 * see shadow_hijack.c's "rmmod safety" note), so an ordinary `rmmod
 * lkm4ctr` already refuses to race with in-flight hooked calls: the kernel
 * simply returns -EBUSY ("Module lkm4ctr is in use") until they drain.
 *
 * That still leaves the operator to retry rmmod by hand until it succeeds.
 * This file adds a sysfs control, /sys/module/lkm4ctr/safe_unload, that
 * automates the whole sequence: writing "1" (or "unload"/"remove") to it
 * quiesces every hook (stopping new in-flight calls from starting), waits
 * for module_refcount() to drain to zero, and then triggers a genuine
 * userspace-driven module removal -- i.e. the module unloads itself, with
 * no further operator action required.
 *
 * The self-unload hazard
 * -----------------------
 * A module can never *directly* free the memory its own currently
 * executing code lives in -- whatever function is doing the freeing would
 * have to keep running afterwards to return to its caller, straight into
 * pages that no longer exist. This is why every practical "self-unload"
 * design (including this one) is actually two cooperating pieces:
 *
 *   1. A worker kthread, spawned by the sysfs write, that does the waiting
 *      (quiesce + poll module_refcount()) and then asks a real userspace
 *      process to do the actual `rmmod`/`modprobe -r` -- module removal is
 *      always driven by an external process calling delete_module(2); no
 *      in-kernel API removes "the currently running module" from within
 *      itself.
 *   2. module_put_and_kthread_exit(), used instead of a normal return from
 *      the worker thread's body once its job is done. This is the one
 *      piece of core kernel code (kernel/module/main.c, *not* our module's
 *      .text) whose entire purpose is to drop the worker's own module
 *      reference and terminate the thread as a single atomic step from the
 *      kernel's point of view, so that no instruction belonging to
 *      lkm4ctr.ko executes after the reference that was keeping the module
 *      alive for the worker's own sake is gone. On kernels old enough to
 *      predate that helper (introduced upstream alongside kthread_exit()
 *      around v5.17, the same threshold shadow_hijack.c's get_kretprobe()
 *      workaround uses), the equivalent classic
 *      module_put_and_exit()/do_exit() pairing is used instead.
 *
 * With both pieces in place, module_refcount() only ever reaches zero
 * after every hooked call *and* the worker thread itself has stopped
 * touching the module's code, so the external `rmmod` this file spawns is
 * operating on a module that is genuinely idle, not one that merely
 * *looks* idle from a racing kthread's perspective.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/kthread.h>
#include <linux/umh.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/sched.h>

#include "shadow_hook.h"

/*
 * Upper bound on how long we wait for module_refcount() to drain before
 * giving up and un-quiescing (so the module keeps working normally and the
 * operator can retry, or investigate a hook that is stuck).
 */
#define LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS	30000
#define LKM4CTR_SAFE_UNLOAD_POLL_MS	50

/*
 * Candidate rmmod-equivalent binaries to try, in order -- mirrors the
 * "try each candidate name until one works" pattern shadow_hook_resolve()
 * uses for symbol names. Android userspace and generic Linux userspace
 * disagree on where (or whether) these live, and some minimal images only
 * ship busybox's applet form.
 */
static const char * const lkm4ctr_rmmod_candidates[] = {
	"/system/bin/rmmod",
	"/sbin/rmmod",
	"/usr/sbin/rmmod",
	"/usr/bin/rmmod",
	"/bin/rmmod",
	NULL,
};

static struct task_struct *lkm4ctr_unload_thread;
static DEFINE_MUTEX(lkm4ctr_unload_lock);
static bool lkm4ctr_unload_in_progress;

/*
 * lkm4ctr_run_rmmod() - exec a real userspace rmmod/modprobe -r process.
 *
 * Tries each candidate path in turn with call_usermodehelper(UMH_WAIT_EXEC),
 * which blocks only until the child has execve()'d successfully (or failed
 * to), not until it exits -- we don't want (and must not) block the
 * kthread on the child's full runtime, since kernel_delete_module() inside
 * that child cannot make progress until module_refcount() is checked
 * again, which the child does entirely on its own schedule.
 *
 * Returns 0 if some candidate was successfully exec'd, or the last
 * candidate's error otherwise.
 */
static int lkm4ctr_run_rmmod(void)
{
	char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/system/bin", NULL };
	const char * const *path;
	int ret = -ENOENT;

	for (path = lkm4ctr_rmmod_candidates; *path; path++) {
		char *argv[] = { (char *)*path, "lkm4ctr", NULL };

		ret = call_usermodehelper(*path, argv, envp, UMH_WAIT_EXEC);
		if (ret == 0) {
			pr_info("lkm4ctr: safe_unload: launched \"%s lkm4ctr\"\n", *path);
			return 0;
		}
		pr_debug("lkm4ctr: safe_unload: \"%s\" failed to exec: %d\n", *path, ret);
	}

	pr_err("lkm4ctr: safe_unload: no rmmod candidate could be exec'd (last error %d); module left quiesced but loaded\n",
	       ret);
	return ret;
}

/*
 * lkm4ctr_safe_unload_fn() - worker kthread body.
 *
 * Quiesces every hook, waits for module_refcount() to drain, and either
 * hands off to a real rmmod (success path, never returns) or un-quiesces
 * and exits normally (failure/timeout path, module keeps working).
 */
static int lkm4ctr_safe_unload_fn(void *unused)
{
	unsigned long waited_ms = 0;
	int ret;

	/*
	 * Pin the module for the duration of this thread's own work. This
	 * reference is what makes module_put_and_kthread_exit() below safe:
	 * it is dropped and the thread torn down as a single step performed
	 * by core kernel code, never by an instruction inside lkm4ctr.ko
	 * itself.
	 */
	__module_get(THIS_MODULE);

	pr_info("lkm4ctr: safe_unload: quiescing hooks\n");
	shadow_hook_quiesce(true);

	/*
	 * module_refcount() is 1 for our own reference just above; wait for
	 * it to drop back to exactly that (i.e. every in-flight hooked call
	 * has released its reference) before proceeding.
	 */
	while (module_refcount(THIS_MODULE) > 1) {
		if (waited_ms >= LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS) {
			pr_err("lkm4ctr: safe_unload: timed out waiting for %d in-flight call(s) to drain; aborting, module remains loaded\n",
			       module_refcount(THIS_MODULE) - 1);
			shadow_hook_quiesce(false);
			goto abort;
		}
		msleep(LKM4CTR_SAFE_UNLOAD_POLL_MS);
		waited_ms += LKM4CTR_SAFE_UNLOAD_POLL_MS;
	}

	pr_info("lkm4ctr: safe_unload: drained, launching rmmod\n");
	ret = lkm4ctr_run_rmmod();
	if (ret) {
		shadow_hook_quiesce(false);
		goto abort;
	}

	/*
	 * Success: a real rmmod process is now on its way to remove us.
	 * Drop our own pin and terminate without ever returning into this
	 * module's .text again -- see the file header for why this specific
	 * helper (rather than a plain `return`) is mandatory here.
	 */
	mutex_lock(&lkm4ctr_unload_lock);
	lkm4ctr_unload_in_progress = false;
	mutex_unlock(&lkm4ctr_unload_lock);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
	module_put_and_kthread_exit(0);
#else
	module_put_and_exit(0);
#endif
	/* NOTREACHED */

abort:
	mutex_lock(&lkm4ctr_unload_lock);
	lkm4ctr_unload_in_progress = false;
	mutex_unlock(&lkm4ctr_unload_lock);
	module_put(THIS_MODULE);
	return 0;
}

static ssize_t safe_unload_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	bool in_progress;

	mutex_lock(&lkm4ctr_unload_lock);
	in_progress = lkm4ctr_unload_in_progress;
	mutex_unlock(&lkm4ctr_unload_lock);

	return sysfs_emit(buf, "%s\n", in_progress ? "in-progress" : "idle");
}

static ssize_t safe_unload_store(struct kobject *kobj, struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	struct task_struct *thread;

	if (!sysfs_streq(buf, "1") && !sysfs_streq(buf, "unload") && !sysfs_streq(buf, "remove"))
		return -EINVAL;

	mutex_lock(&lkm4ctr_unload_lock);
	if (lkm4ctr_unload_in_progress) {
		mutex_unlock(&lkm4ctr_unload_lock);
		return -EBUSY;
	}
	lkm4ctr_unload_in_progress = true;
	mutex_unlock(&lkm4ctr_unload_lock);

	thread = kthread_run(lkm4ctr_safe_unload_fn, NULL, "lkm4ctr_unload");
	if (IS_ERR(thread)) {
		mutex_lock(&lkm4ctr_unload_lock);
		lkm4ctr_unload_in_progress = false;
		mutex_unlock(&lkm4ctr_unload_lock);
		return PTR_ERR(thread);
	}
	lkm4ctr_unload_thread = thread;

	return count;
}

static struct kobj_attribute lkm4ctr_safe_unload_attr = __ATTR_RW(safe_unload);

static struct attribute *lkm4ctr_sysfs_attrs[] = {
	&lkm4ctr_safe_unload_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(lkm4ctr_sysfs);

int lkm4ctr_safe_unload_init(void)
{
	/*
	 * Hang safe_unload off THIS_MODULE's own kobject
	 * (/sys/module/lkm4ctr/) rather than creating a new one under
	 * kernel_kobj. kernel_kobj is EXPORT_SYMBOL_GPL()'d but, like
	 * ftrace_set_filter_ip()/vm_mmap()/anon_inode_getfd_secure()
	 * elsewhere in this module, it is a data symbol with no built-in
	 * (non-modular) callers on some production GKI kernels and so gets
	 * stripped by CONFIG_TRIM_UNUSED_KSYMS -- causing insmod to fail
	 * with "Unknown symbol kernel_kobj". Unlike those function symbols,
	 * a data symbol can't be recovered via shadow_hook_resolve()'s
	 * register_kprobe() trick (kprobes only accept text addresses), so
	 * the fix here is to avoid needing kernel_kobj at all:
	 * THIS_MODULE->mkobj.kobj is set up by the module loader itself
	 * (mod_sysfs_setup(), before our module_init runs) and requires no
	 * export whatsoever.
	 */
	return sysfs_create_groups(&THIS_MODULE->mkobj.kobj, lkm4ctr_sysfs_groups);
}

void lkm4ctr_safe_unload_exit(void)
{
	sysfs_remove_groups(&THIS_MODULE->mkobj.kobj, lkm4ctr_sysfs_groups);
}
