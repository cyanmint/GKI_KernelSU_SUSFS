// SPDX-License-Identifier: GPL-2.0
/*
 * lkm4ctr_diagfs - the "lkm4ctr" pseudo-filesystem: `mount -t lkm4ctr diag
 * ./mnt` exposes a read/write diagnostics tree replacing the earlier
 * /dev/lkm4ctr_safe_unload misc device entirely (see lkm4ctr_safe_unload.c's
 * removal), plus the "which modules are activated, what hooks/namespaces
 * are currently in use" introspection that misc device never offered:
 *
 *   ./mnt/modules/<subsystem>/status      - "active"/"not loaded"; echo
 *                                            "load"/"unload" force-installs
 *                                            or force-removes that
 *                                            submodule's hooks at runtime.
 *   ./mnt/modules/<subsystem>/hooks       - one line per currently
 *                                            registered hook: resolved
 *                                            symbol name, installed state,
 *                                            address.
 *   ./mnt/modules/shadow_ns/namespaces    - the full shadow_ns namespace
 *                                            registry and which task group
 *                                            (tgid) currently sits in which
 *                                            namespace of each type.
 *   ./mnt/modules/<subsystem>/log         - that submodule's log lines.
 *   ./mnt/log                             - every log line, unfiltered.
 *   ./mnt/safe_unload                     - read: current safe-unload
 *                                            progress plus its own log
 *                                            tail; write "1"/"unload"/
 *                                            "remove": start the safe
 *                                            self-unload sequence (see
 *                                            lkm4ctr_safe_unload_worker()
 *                                            below for the full rationale,
 *                                            carried over unchanged from
 *                                            the removed misc device).
 *                                            NOTE: like any filesystem
 *                                            module, lkm4ctr.ko cannot
 *                                            actually unload while any
 *                                            "lkm4ctr" mount is still
 *                                            active (file_system_type's
 *                                            .owner keeps module_refcount()
 *                                            non-zero for as long as it is
 *                                            mounted anywhere, the same way
 *                                            it would refuse rmmod on any
 *                                            other in-use filesystem
 *                                            module) -- the worker
 *                                            auto-unmounts every active
 *                                            "lkm4ctr" mount itself (see
 *                                            lkm4ctr_auto_umount_diagfs())
 *                                            after quiescing hooks and
 *                                            before waiting for
 *                                            module_refcount() to drain, so
 *                                            no manual umount is normally
 *                                            required; if a mount still
 *                                            cannot be cleared (no umount
 *                                            binary present, or something
 *                                            keeps re-mounting it), the
 *                                            worker will time out and log
 *                                            exactly what is still blocking
 *                                            it plus how to resolve it.
 *
 * Implementation
 * --------------
 * This is a small, fully in-memory pseudo-filesystem in the same spirit as
 * ramfs/securityfs: every dentry/inode in the tree is created once, up
 * front, at mount time (mount_nodev() + fill_super()); nothing is created
 * or destroyed lazily afterwards (status/log/hooks/namespaces content is
 * regenerated fresh on every open() via a seq_file single_open(), not
 * stored). Directory traversal reuses the kernel's own
 * simple_dir_inode_operations/simple_dir_operations (fs/libfs.c) rather
 * than hand-rolling dcache walking: unlike function symbols such as
 * ftrace_set_filter_ip()/vm_mmap()/module_refcount() elsewhere in this
 * module, these two are plain data (struct) symbols, so they cannot be
 * recovered via shadow_hook_resolve()'s register_kprobe() trick if
 * CONFIG_TRIM_UNUSED_KSYMS ever stripped them -- but they are directly
 * referenced by security/inode.c (securityfs), which every Android GKI
 * kernel builds directly into vmlinux (CONFIG_SECURITYFS=y, a hard
 * requirement of the SELinux LSM every such kernel enables), giving them a
 * permanent non-modular in-tree caller that CONFIG_TRIM_UNUSED_KSYMS can
 * never trim away. This is the same "always-referenced-by-something-
 * essential" reasoning already relied on for misc_register()/
 * misc_deregister() (see the removed lkm4ctr_safe_unload.c's file header).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/kthread.h>
#include <linux/umh.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/atomic.h>

#include "shadow_hook.h"
#include "lkm4ctr_log.h"
#include "lkm4ctr_compat.h"

#define LKM4CTR_DIAGFS_MAGIC	0x4C4B4D34 /* "LKM4" */
#define LKM4CTR_DIAGFS_TAG	"diagfs"

/*
 * mount_nodev() and generic_delete_inode() are ordinary EXPORT_SYMBOL()
 * functions, not GPL-only, but unlike simple_dir_inode_operations/
 * simple_dir_operations above they are plain code, not data -- so they
 * *can* be recovered via shadow_hook_resolve()'s register_kprobe() trick
 * if CONFIG_TRIM_UNUSED_KSYMS strips them (confirmed: "Unknown symbol
 * mount_nodev"/"Unknown symbol generic_delete_inode" on insmod against a
 * production GKI kernel), same class of issue as module_refcount()/
 * call_usermodehelper() below. Resolved lazily at lkm4ctr_diagfs_init()
 * time instead of calling them directly.
 */
typedef struct dentry *(*lkm4ctr_mount_nodev_t)(struct file_system_type *fs_type,
						 int flags, void *data,
						 int (*fill_super)(struct super_block *,
								    void *, int));
typedef int (*lkm4ctr_generic_delete_inode_t)(struct inode *inode);

static lkm4ctr_mount_nodev_t lkm4ctr_mount_nodev_fn;
static lkm4ctr_generic_delete_inode_t lkm4ctr_generic_delete_inode_fn;

/* shadow_ns_diag.c; deliberately forward-declared rather than pulling in
 * shadow_ns_internal.h, matching how lkm4ctr_main.c forward-declares every
 * other subsystem's init/exit entry points instead of including private
 * per-subsystem headers.
 */
extern size_t shadow_ns_diag_snprintf(char *buf, size_t buflen);

enum lkm4ctr_diagfs_kind {
	LKM4CTR_DIAG_STATUS,
	LKM4CTR_DIAG_HOOKS,
	LKM4CTR_DIAG_NAMESPACES,
	LKM4CTR_DIAG_LOG,
	LKM4CTR_DIAG_SAFE_UNLOAD,
};

struct lkm4ctr_diagfs_info {
	enum lkm4ctr_diagfs_kind	kind;
	char				tag[LKM4CTR_LOG_TAG_MAX];
};

struct lkm4ctr_diagfs_module {
	const char	*dirname;
	const char	*tag;
	bool		has_hooks;
	bool		has_namespaces;
};

/*
 * The submodules the diagnostics tree enumerates. "shadow_hijack" is the
 * shared hook engine every other entry depends on: it owns no hooks or
 * namespaces of its own and is always active for as long as lkm4ctr.ko
 * itself is loaded, so it gets a status/log pair only.
 */
static const struct lkm4ctr_diagfs_module lkm4ctr_diagfs_modules[] = {
	{ "shadow_hijack",	"shadow_hijack",	false,	false },
	{ "shadow_ns",		"shadow_ns",		true,	true  },
	{ "shadow_sysvipc",	"shadow_sysvipc",	true,	false },
	{ "shadow_mqueue",	"shadow_mqueue",	true,	false },
	{ "shadow_cgdevices",	"shadow_cgdevices",	true,	false },
};

/* ------------------------------------------------------------------- */
/* inode/dentry tree construction                                       */
/* ------------------------------------------------------------------- */

static const struct file_operations lkm4ctr_diagfs_ro_fops;
static const struct file_operations lkm4ctr_diagfs_status_fops;
static const struct file_operations lkm4ctr_diagfs_safe_unload_fops;

static struct inode *lkm4ctr_diagfs_make_inode(struct super_block *sb, umode_t mode)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_uid = GLOBAL_ROOT_UID;
	inode->i_gid = GLOBAL_ROOT_GID;
	lkm4ctr_inode_init_ts(inode);

	return inode;
}

static struct dentry *lkm4ctr_diagfs_mkdir(struct super_block *sb,
					    struct dentry *parent,
					    const char *name)
{
	struct inode *inode;
	struct dentry *dentry;

	inode = lkm4ctr_diagfs_make_inode(sb, S_IFDIR | 0555);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	set_nlink(inode, 2);

	inode_lock(d_inode(parent));
	dentry = d_alloc_name(parent, name);
	if (!dentry) {
		inode_unlock(d_inode(parent));
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	d_add(dentry, inode);
	inc_nlink(d_inode(parent));
	inode_unlock(d_inode(parent));

	return dentry;
}

static struct dentry *lkm4ctr_diagfs_create_file(struct super_block *sb,
						  struct dentry *parent,
						  const char *name, umode_t mode,
						  enum lkm4ctr_diagfs_kind kind,
						  const char *tag)
{
	struct inode *inode;
	struct dentry *dentry;
	struct lkm4ctr_diagfs_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);
	info->kind = kind;
	if (tag)
		strscpy(info->tag, tag, sizeof(info->tag));

	inode = lkm4ctr_diagfs_make_inode(sb, S_IFREG | mode);
	if (!inode) {
		kfree(info);
		return ERR_PTR(-ENOMEM);
	}
	inode->i_private = info;

	switch (kind) {
	case LKM4CTR_DIAG_SAFE_UNLOAD:
		inode->i_fop = &lkm4ctr_diagfs_safe_unload_fops;
		break;
	case LKM4CTR_DIAG_STATUS:
		inode->i_fop = &lkm4ctr_diagfs_status_fops;
		break;
	default:
		inode->i_fop = &lkm4ctr_diagfs_ro_fops;
		break;
	}

	inode_lock(d_inode(parent));
	dentry = d_alloc_name(parent, name);
	if (!dentry) {
		inode_unlock(d_inode(parent));
		iput(inode);
		kfree(info);
		return ERR_PTR(-ENOMEM);
	}
	d_add(dentry, inode);
	inode_unlock(d_inode(parent));

	return dentry;
}

/* ------------------------------------------------------------------- */
/* content rendering (status/hooks/namespaces/log)                     */
/* ------------------------------------------------------------------- */

static size_t lkm4ctr_diagfs_status_snprintf(const char *tag, char *buf, size_t buflen)
{
	bool active;

	/*
	 * shadow_hijack is the shared hook engine linked into every other
	 * submodule; it registers no hooks of its own, so the generic
	 * registry has nothing to report for it. It is active for exactly
	 * as long as lkm4ctr.ko is loaded, which is always true by the time
	 * anything can read this file.
	 */
	if (!strcmp(tag, "shadow_hijack"))
		active = true;
	else
		active = shadow_hook_registry_tag_active(tag);

	return scnprintf(buf, buflen, "%s\n", active ? "active" : "not loaded");
}

static size_t lkm4ctr_diagfs_namespaces_snprintf(const char *tag, char *buf, size_t buflen)
{
	return shadow_ns_diag_snprintf(buf, buflen);
}

typedef size_t (*lkm4ctr_diagfs_render_fn)(const char *tag, char *buf, size_t buflen);

static lkm4ctr_diagfs_render_fn lkm4ctr_diagfs_render_for(enum lkm4ctr_diagfs_kind kind)
{
	switch (kind) {
	case LKM4CTR_DIAG_STATUS:
		return lkm4ctr_diagfs_status_snprintf;
	case LKM4CTR_DIAG_HOOKS:
		return shadow_hook_registry_snprintf;
	case LKM4CTR_DIAG_NAMESPACES:
		return lkm4ctr_diagfs_namespaces_snprintf;
	case LKM4CTR_DIAG_LOG:
		return lkm4ctr_log_snprintf;
	default:
		return NULL;
	}
}

/*
 * lkm4ctr_diagfs_show() - seq_file show callback shared by every read-only
 * (and the readable half of read/write) diagfs file. Regenerates full
 * content fresh on every open(), sized to fit via the same "try, grow,
 * retry" loop every renderer's snprintf()-style contract expects.
 */
static int lkm4ctr_diagfs_show(struct seq_file *m, void *v)
{
	struct lkm4ctr_diagfs_info *info = m->private;
	lkm4ctr_diagfs_render_fn fn = lkm4ctr_diagfs_render_for(info->kind);
	char *buf;
	size_t cap = 4096, need;

	if (!fn)
		return -EINVAL;

	for (;;) {
		buf = kmalloc(cap, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		/* info->tag is "" for the root-level (all-modules) log/hooks files; treat as NULL ("no filter"). */
		need = fn(info->tag[0] != '\0' ? info->tag : NULL, buf, cap);
		if (need < cap)
			break;
		kfree(buf);
		cap = need + 1;
	}

	seq_write(m, buf, need);
	kfree(buf);
	return 0;
}

static int lkm4ctr_diagfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, lkm4ctr_diagfs_show, inode->i_private);
}

static const struct file_operations lkm4ctr_diagfs_ro_fops = {
	.owner		= THIS_MODULE,
	.open		= lkm4ctr_diagfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ------------------------------------------------------------------- */
/* status: echo load/unload > .../status                                */
/* ------------------------------------------------------------------- */

static ssize_t lkm4ctr_diagfs_status_write(struct file *file, const char __user *ubuf,
					    size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct lkm4ctr_diagfs_info *info = m->private;
	char cmd[16];
	bool enable;
	int ret;

	if (count == 0 || count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';
	strim(cmd);

	if (!strcmp(cmd, "load"))
		enable = true;
	else if (!strcmp(cmd, "unload"))
		enable = false;
	else
		return -EINVAL;

	if (!strcmp(info->tag, "shadow_hijack")) {
		LKM4CTR_WARN(info->tag,
			     "shadow_hijack is the shared hook engine; it cannot be force-loaded/unloaded independently");
		return -EOPNOTSUPP;
	}

	LKM4CTR_INFO(info->tag, "%s requested via diagfs status write", enable ? "load" : "unload");

	ret = shadow_hook_registry_set_active(info->tag, enable);
	if (ret) {
		LKM4CTR_ERR(info->tag,
			    "%s request failed (%d); see the messages just above in this same log for exactly which hook group and underlying error blocked it, and %s/log for the full log",
			    enable ? "load" : "unload", ret, info->tag);
		return ret;
	}

	LKM4CTR_INFO(info->tag, "%s forced via diagfs status", enable ? "load" : "unload");
	return count;
}

static const struct file_operations lkm4ctr_diagfs_status_fops = {
	.owner		= THIS_MODULE,
	.open		= lkm4ctr_diagfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= lkm4ctr_diagfs_status_write,
};

/* ------------------------------------------------------------------- */
/* safe_unload -- ported from the removed /dev/lkm4ctr_safe_unload misc  */
/* device; see below for the full self-unload rationale (unchanged).    */
/* ------------------------------------------------------------------- */

/*
 * The self-unload hazard
 * -----------------------
 * A module can never *directly* free the memory its own currently
 * executing code lives in -- whatever function is doing the freeing would
 * have to keep running afterwards to return to its caller, straight into
 * pages that no longer exist. This is why every practical "self-unload"
 * design (including this one) is actually two cooperating pieces:
 *
 *   1. A worker kthread, spawned by the write() below, that does the
 *      waiting (quiesce + poll module_refcount()) and then asks a real
 *      userspace process to do the actual `rmmod`/`modprobe -r` -- module
 *      removal is always driven by an external process calling
 *      delete_module(2); no in-kernel API removes "the currently running
 *      module" from within itself.
 *   2. module_put_and_kthread_exit(), used instead of a normal return from
 *      the worker thread's body once its job is done. This is the one
 *      piece of core kernel code (kernel/module/main.c, *not* our module's
 *      .text) whose entire purpose is to drop the worker's own module
 *      reference and terminate the thread as a single atomic step from the
 *      kernel's point of view, so that no instruction belonging to
 *      lkm4ctr.ko executes after the reference that was keeping the module
 *      alive for the worker's own sake is gone. On kernels old enough to
 *      predate that helper (introduced upstream alongside kthread_exit()
 *      around v5.17), the equivalent classic module_put_and_exit()/
 *      do_exit() pairing is used instead.
 *
 * With both pieces in place, module_refcount() only ever reaches zero
 * after every hooked call *and* the worker thread itself has stopped
 * touching the module's code, so the external `rmmod` this file spawns is
 * operating on a module that is genuinely idle, not one that merely
 * *looks* idle from a racing kthread's perspective.
 */

#define LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS	30000
#define LKM4CTR_SAFE_UNLOAD_POLL_MS	50
#define LKM4CTR_SAFE_UNLOAD_TAG		"safe_unload"

static const char * const lkm4ctr_rmmod_candidates[] = {
	"/system/bin/rmmod",
	"/sbin/rmmod",
	"/usr/sbin/rmmod",
	"/usr/bin/rmmod",
	"/bin/rmmod",
	NULL,
};

static const char * const lkm4ctr_umount_candidates[] = {
	"/system/bin/umount",
	"/sbin/umount",
	"/usr/sbin/umount",
	"/usr/bin/umount",
	"/bin/umount",
	NULL,
};

static struct task_struct *lkm4ctr_unload_thread;
static DEFINE_MUTEX(lkm4ctr_unload_lock);
static bool lkm4ctr_unload_in_progress;

/*
 * Number of currently active "lkm4ctr" mounts (incremented in
 * lkm4ctr_diagfs_fill_super(), decremented in lkm4ctr_diagfs_kill_sb()
 * below). Read by the safe_unload worker's timeout path so a stuck
 * self-unload can be diagnosed precisely instead of just reporting a bare
 * refcount number: a mounted diagfs is by far the most common reason
 * module_refcount() never drains, since file_system_type->owner pins it
 * for as long as any instance is mounted anywhere.
 */
static atomic_t lkm4ctr_diagfs_mount_count = ATOMIC_INIT(0);

/*
 * module_refcount() and call_usermodehelper() are ordinary EXPORT_SYMBOL()
 * functions, not GPL-only, but that alone doesn't save them from
 * CONFIG_TRIM_UNUSED_KSYMS on production GKI kernels: like
 * ftrace_set_filter_ip()/vm_mmap()/anon_inode_getfd_secure() elsewhere in
 * this module, they get stripped whenever nothing built into vmlinux
 * itself calls them. Resolved lazily via shadow_hook_resolve() instead,
 * same as those other trimmed symbols.
 */
typedef int (*lkm4ctr_module_refcount_t)(struct module *mod);
typedef int (*lkm4ctr_call_usermodehelper_t)(const char *path, char **argv,
					      char **envp, int wait);

static lkm4ctr_module_refcount_t lkm4ctr_module_refcount_fn;
static lkm4ctr_call_usermodehelper_t lkm4ctr_call_usermodehelper_fn;

#define LKM4CTR_RESOLVE_ONE(fn, name)						\
	do {									\
		if (!(fn)) {							\
			(fn) = (typeof(fn))shadow_hook_resolve(name);		\
			if (!(fn))						\
				LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,		\
					     "could not resolve %s; safe_unload unavailable", \
					     name);				\
		}								\
	} while (0)

static bool lkm4ctr_safe_unload_resolve(void)
{
	LKM4CTR_RESOLVE_ONE(lkm4ctr_module_refcount_fn, "module_refcount");
	LKM4CTR_RESOLVE_ONE(lkm4ctr_call_usermodehelper_fn, "call_usermodehelper");

	return lkm4ctr_module_refcount_fn && lkm4ctr_call_usermodehelper_fn;
}

/*
 * lkm4ctr_auto_umount_diagfs() - best-effort automatic unmount of every
 * active "lkm4ctr" diagfs mount, run by the safe_unload worker after hooks
 * have been quiesced and before it starts waiting for module_refcount() to
 * drain / launches rmmod. A mounted diagfs pins module_refcount() via
 * file_system_type->owner (see lkm4ctr_diagfs_kill_sb()'s comment), so
 * without this the operator would always have to remember to `umount` it
 * by hand first.
 *
 * There is no safe, exported, general "force unmount this fstype from
 * every mount namespace" kernel API -- the mount tree (struct mount,
 * lock_mount_hash(), umount_tree(), ...) is private to fs/namespace.c and
 * not exported for out-of-tree use, and iterate_supers_type() only reaches
 * the shared superblock, not the per-namespace mountpoint path needed to
 * actually detach it. So, like the rmmod launch itself, this shells out to
 * a real "umount" userspace helper via call_usermodehelper() with "-a -t
 * lkm4ctr -l": "-a -t lkm4ctr" finds every active lkm4ctr mountpoint from
 * /proc/mounts without the kernel needing to know its path, and "-l"
 * (lazy/MNT_DETACH) still detaches it from the mount tree immediately even
 * if a process's cwd or an open fd is still inside it (e.g. the very shell
 * that just did `echo 1 > .../safe_unload`) -- the underlying superblock
 * (and this module's pinned reference to it) is then released as soon as
 * that last reference itself goes away, rather than requiring the operator
 * to have already cd'ed out first.
 */
static int lkm4ctr_auto_umount_diagfs(void)
{
	char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/system/bin", NULL };
	const char * const *path;
	int mounts = atomic_read(&lkm4ctr_diagfs_mount_count);
	int ret = -ENOENT;

	if (mounts <= 0) {
		LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
			     "no lkm4ctr diagfs mount currently active, nothing to auto-unmount");
		return 0;
	}

	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
		     "auto-unmounting %d active lkm4ctr diagfs mount(s) before self-unload", mounts);

	for (path = lkm4ctr_umount_candidates; *path; path++) {
		char *argv[] = { (char *)*path, "-l", "-a", "-t", "lkm4ctr", NULL };

		ret = lkm4ctr_call_usermodehelper_fn(*path, argv, envp, UMH_WAIT_PROC);
		if (ret == 0) {
			LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG, "ran \"%s -l -a -t lkm4ctr\"", *path);
			break;
		}
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "\"%s -l -a -t lkm4ctr\" failed or exited non-zero: %d", *path, ret);
	}

	mounts = atomic_read(&lkm4ctr_diagfs_mount_count);
	if (mounts > 0) {
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "%d lkm4ctr diagfs mount(s) still marked active after the auto-unmount attempt",
			     mounts);
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "cause: no working umount binary was found on this system, or the lazy-detach superblock is still waiting on the last held reference to actually drop");
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG,
			     "resolution: self-unload will time out below unless these are cleared -- see the timeout guidance further down this log for exact resolution steps");
	} else {
		LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG, "all lkm4ctr diagfs mounts successfully auto-unmounted");
	}

	return ret;
}

static int lkm4ctr_run_rmmod(void)
{
	char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin:/system/bin", NULL };
	const char * const *path;
	int ret = -ENOENT;

	for (path = lkm4ctr_rmmod_candidates; *path; path++) {
		char *argv[] = { (char *)*path, "lkm4ctr", NULL };

		ret = lkm4ctr_call_usermodehelper_fn(*path, argv, envp, UMH_WAIT_EXEC);
		if (ret == 0) {
			LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG, "launched \"%s lkm4ctr\"", *path);
			return 0;
		}
		LKM4CTR_WARN(LKM4CTR_SAFE_UNLOAD_TAG, "\"%s\" failed to exec: %d", *path, ret);
	}

	LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG, "no rmmod candidate could be exec'd (last error %d)", ret);
	LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
		    "cause: module was successfully quiesced and drained, so the module itself is not the problem -- likely no rmmod (or busybox applet providing it) is installed/executable on this system");
	LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
		    "resolution: ensure a working rmmod exists and is executable on one of /system/bin, /sbin, /usr/sbin, /usr/bin, /bin, then retry -- or simply run `rmmod lkm4ctr` by hand right now, it will succeed immediately since hooks are already quiesced and the refcount is already drained; module left quiesced but loaded");
	return ret;
}

static int lkm4ctr_safe_unload_fn(void *unused)
{
	unsigned long waited_ms = 0;
	int ret;
	int refcount;

	__module_get(THIS_MODULE);

	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
		     "safe unload started: quiescing hooks (in-flight shadow_hook-redirected syscalls are allowed to finish, new entries are refused with -EAGAIN)");
	shadow_hook_quiesce(true);

	lkm4ctr_auto_umount_diagfs();

	refcount = lkm4ctr_module_refcount_fn(THIS_MODULE);
	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
		     "module_refcount()=%d after quiesce (must drop to 1, i.e. only this worker's own reference, before rmmod can succeed; timeout is %ums)",
		     refcount, LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS);

	while ((refcount = lkm4ctr_module_refcount_fn(THIS_MODULE)) > 1) {
		if (waited_ms >= LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS) {
			int mounts = atomic_read(&lkm4ctr_diagfs_mount_count);

			LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
				    "timed out after %lums waiting for %d extra reference(s) to drain (module_refcount()=%d); aborting, module remains loaded",
				    waited_ms, refcount - 1, refcount);
			if (mounts > 0) {
				LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
					    "cause: this lkm4ctr diagfs is currently mounted %d time(s); every active mount pins module_refcount() via file_system_type->owner, exactly like rmmod refuses any other in-use filesystem module, and this alone will block self-unload forever",
					    mounts);
				LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
					    "resolution: `umount` every mountpoint of type \"lkm4ctr\" (check with `grep lkm4ctr /proc/mounts`) -- including the one you may be reading/writing safe_unload through right now -- then write to safe_unload again");
			} else {
				LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
					    "cause: %d extra reference(s) remain with no lkm4ctr diagfs mounted, so a shadow_hook-redirected syscall is most likely still executing in another task, or a resource created via a hook (e.g. an anon-inode fd) is still held open",
					    refcount - 1);
				LKM4CTR_ERR(LKM4CTR_SAFE_UNLOAD_TAG,
					    "resolution: check ./mnt/modules/<subsystem>/hooks and ./mnt/modules/shadow_ns/namespaces for tasks/namespaces still in use by shadow_ns/shadow_sysvipc/shadow_mqueue/shadow_cgdevices, let those operations finish or terminate the owning processes, then retry; if the count never drops on retry this may be a reference leak worth reporting together with that hooks/namespaces output");
			}
			shadow_hook_quiesce(false);
			goto abort;
		}
		if (waited_ms && waited_ms % 5000 == 0)
			LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
				     "still waiting after %lums: module_refcount()=%d (%d extra reference(s) remaining, %lums until timeout)",
				     waited_ms, refcount, refcount - 1,
				     LKM4CTR_SAFE_UNLOAD_TIMEOUT_MS - waited_ms);
		msleep(LKM4CTR_SAFE_UNLOAD_POLL_MS);
		waited_ms += LKM4CTR_SAFE_UNLOAD_POLL_MS;
	}

	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG,
		     "drained after %lums (module_refcount()=%d), launching rmmod", waited_ms, refcount);
	ret = lkm4ctr_run_rmmod();
	if (ret) {
		shadow_hook_quiesce(false);
		goto abort;
	}

	LKM4CTR_INFO(LKM4CTR_SAFE_UNLOAD_TAG, "rmmod launched successfully, module is unloading");

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

static size_t lkm4ctr_safe_unload_snprintf(const char *tag, char *buf, size_t buflen)
{
	bool in_progress;
	size_t pos;

	mutex_lock(&lkm4ctr_unload_lock);
	in_progress = lkm4ctr_unload_in_progress;
	mutex_unlock(&lkm4ctr_unload_lock);

	pos = scnprintf(buf, buflen, "state: %s\n", in_progress ? "in-progress" : "idle");
	pos += lkm4ctr_log_snprintf(LKM4CTR_SAFE_UNLOAD_TAG, buf + pos,
				     pos < buflen ? buflen - pos : 0);
	return pos;
}

static ssize_t lkm4ctr_diagfs_safe_unload_write(struct file *file, const char __user *ubuf,
						 size_t count, loff_t *ppos)
{
	char cmd[16];
	struct task_struct *thread;

	if (count == 0 || count >= sizeof(cmd))
		return -EINVAL;

	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';
	strim(cmd);

	if (strcmp(cmd, "1") && strcmp(cmd, "unload") && strcmp(cmd, "remove"))
		return -EINVAL;

	if (!lkm4ctr_safe_unload_resolve())
		return -EOPNOTSUPP;

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

static int lkm4ctr_diagfs_safe_unload_show(struct seq_file *m, void *v)
{
	char *buf;
	size_t cap = 4096, need;

	for (;;) {
		buf = kmalloc(cap, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		need = lkm4ctr_safe_unload_snprintf(NULL, buf, cap);
		if (need < cap)
			break;
		kfree(buf);
		cap = need + 1;
	}

	seq_write(m, buf, need);
	kfree(buf);
	return 0;
}

static int lkm4ctr_diagfs_safe_unload_open(struct inode *inode, struct file *file)
{
	return single_open(file, lkm4ctr_diagfs_safe_unload_show, inode->i_private);
}

static const struct file_operations lkm4ctr_diagfs_safe_unload_fops = {
	.owner		= THIS_MODULE,
	.open		= lkm4ctr_diagfs_safe_unload_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= lkm4ctr_diagfs_safe_unload_write,
};

/* ------------------------------------------------------------------- */
/* superblock / filesystem_type registration                           */
/* ------------------------------------------------------------------- */

static void lkm4ctr_diagfs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	kfree(inode->i_private);
}

/*
 * Thin wrapper so super_ops.drop_inode can have a compile-time-known
 * address even though the real generic_delete_inode() is only resolved at
 * runtime (see lkm4ctr_generic_delete_inode_fn's comment above).
 * generic_delete_inode() unconditionally returns 1 (always delete), so
 * that is exactly what this falls back to if resolution somehow never ran
 * (lkm4ctr_diagfs_init() refuses to register the filesystem in that case,
 * so this should never actually be reached).
 */
static int lkm4ctr_diagfs_drop_inode(struct inode *inode)
{
	if (lkm4ctr_generic_delete_inode_fn)
		return lkm4ctr_generic_delete_inode_fn(inode);
	return 1;
}

static const struct super_operations lkm4ctr_diagfs_super_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= lkm4ctr_diagfs_drop_inode,
	.evict_inode	= lkm4ctr_diagfs_evict_inode,
};

static int lkm4ctr_diagfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root_inode;
	struct dentry *modules_dir;
	unsigned int i;

	/*
	 * Counted here (rather than only on success) and unconditionally
	 * balanced by lkm4ctr_diagfs_kill_sb() below: the VFS always calls
	 * ->kill_sb() to unwind a superblock once sb->s_type has been set by
	 * mount_nodev(), even if this function returns an error partway
	 * through, so incrementing exactly once per fill_super() call keeps
	 * the two sides matched regardless of success or failure.
	 */
	atomic_inc(&lkm4ctr_diagfs_mount_count);

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = LKM4CTR_DIAGFS_MAGIC;
	sb->s_op = &lkm4ctr_diagfs_super_ops;
	sb->s_time_gran = 1;

	root_inode = lkm4ctr_diagfs_make_inode(sb, S_IFDIR | 0555);
	if (!root_inode)
		return -ENOMEM;
	root_inode->i_op = &simple_dir_inode_operations;
	root_inode->i_fop = &simple_dir_operations;
	set_nlink(root_inode, 2);

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root)
		return -ENOMEM;

	modules_dir = lkm4ctr_diagfs_mkdir(sb, sb->s_root, "modules");
	if (IS_ERR(modules_dir))
		return PTR_ERR(modules_dir);

	for (i = 0; i < ARRAY_SIZE(lkm4ctr_diagfs_modules); i++) {
		const struct lkm4ctr_diagfs_module *m = &lkm4ctr_diagfs_modules[i];
		struct dentry *dir;

		dir = lkm4ctr_diagfs_mkdir(sb, modules_dir, m->dirname);
		if (IS_ERR(dir))
			return PTR_ERR(dir);

		if (IS_ERR(lkm4ctr_diagfs_create_file(sb, dir, "status", 0644,
						       LKM4CTR_DIAG_STATUS, m->tag)))
			return -ENOMEM;

		if (m->has_hooks &&
		    IS_ERR(lkm4ctr_diagfs_create_file(sb, dir, "hooks", 0444,
						       LKM4CTR_DIAG_HOOKS, m->tag)))
			return -ENOMEM;

		if (m->has_namespaces &&
		    IS_ERR(lkm4ctr_diagfs_create_file(sb, dir, "namespaces", 0444,
						       LKM4CTR_DIAG_NAMESPACES, m->tag)))
			return -ENOMEM;

		if (IS_ERR(lkm4ctr_diagfs_create_file(sb, dir, "log", 0444,
						       LKM4CTR_DIAG_LOG, m->tag)))
			return -ENOMEM;
	}

	if (IS_ERR(lkm4ctr_diagfs_create_file(sb, sb->s_root, "safe_unload", 0644,
					       LKM4CTR_DIAG_SAFE_UNLOAD, NULL)))
		return -ENOMEM;

	if (IS_ERR(lkm4ctr_diagfs_create_file(sb, sb->s_root, "log", 0444,
					       LKM4CTR_DIAG_LOG, NULL)))
		return -ENOMEM;

	return 0;
}

static struct dentry *lkm4ctr_diagfs_mount(struct file_system_type *fs_type,
					    int flags, const char *dev_name, void *data)
{
	if (!lkm4ctr_mount_nodev_fn)
		return ERR_PTR(-ENOSYS);
	return lkm4ctr_mount_nodev_fn(fs_type, flags, data, lkm4ctr_diagfs_fill_super);
}

/*
 * lkm4ctr_diagfs_kill_sb() - balances lkm4ctr_diagfs_fill_super()'s
 * atomic_inc(&lkm4ctr_diagfs_mount_count) above. Tracking this is what lets
 * the safe_unload worker's timeout/failure path (below) tell the operator
 * "the diagfs itself is still mounted N time(s), which is what's keeping
 * module_refcount() non-zero" instead of leaving them to guess.
 */
static void lkm4ctr_diagfs_kill_sb(struct super_block *sb)
{
	atomic_dec(&lkm4ctr_diagfs_mount_count);
	kill_litter_super(sb);
}

static struct file_system_type lkm4ctr_diagfs_type = {
	.owner		= THIS_MODULE,
	.name		= "lkm4ctr",
	.mount		= lkm4ctr_diagfs_mount,
	.kill_sb	= lkm4ctr_diagfs_kill_sb,
};

int lkm4ctr_diagfs_init(void)
{
	int ret;

	lkm4ctr_mount_nodev_fn =
		(lkm4ctr_mount_nodev_t)shadow_hook_resolve("mount_nodev");
	lkm4ctr_generic_delete_inode_fn =
		(lkm4ctr_generic_delete_inode_t)shadow_hook_resolve("generic_delete_inode");

	if (!lkm4ctr_mount_nodev_fn || !lkm4ctr_generic_delete_inode_fn) {
		LKM4CTR_ERR(LKM4CTR_DIAGFS_TAG,
			    "could not resolve mount_nodev/generic_delete_inode; diagfs unavailable");
		return -ENOSYS;
	}

	ret = register_filesystem(&lkm4ctr_diagfs_type);
	if (ret)
		LKM4CTR_ERR(LKM4CTR_DIAGFS_TAG, "register_filesystem() failed: %d", ret);
	else
		LKM4CTR_INFO(LKM4CTR_DIAGFS_TAG,
			     "registered; mount -t lkm4ctr diag <mountpoint> for diagnostics");
	return ret;
}

void lkm4ctr_diagfs_exit(void)
{
	unregister_filesystem(&lkm4ctr_diagfs_type);
}
