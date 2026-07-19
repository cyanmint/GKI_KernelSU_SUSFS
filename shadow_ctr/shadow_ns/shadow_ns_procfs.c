// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns_procfs - fabricate /proc/<pid>/setgroups on kernels genuinely
 * missing CONFIG_USER_NS.
 *
 * fs/proc/base.c only wires up the "setgroups" (and uid_map/gid_map)
 * per-pid dentries when the kernel is built with CONFIG_USER_NS=y (see
 * tgid_base_stuff[]/tid_base_stuff[] in that file, both entries gated by
 * "#ifdef CONFIG_USER_NS"). On a kernel where CONFIG_USER_NS is genuinely
 * absent at runtime -- i.e. shadow_ns is already simulating CLONE_NEWUSER
 * for getuid()/geteuid()/... -- opening that path fails with plain -ENOENT.
 *
 * Modern runc/containerd unconditionally open()/openat2() their own
 * process's "self/setgroups" (frequently relative to a private, detached
 * `fsopen("proc")`+`fsmount()` descriptor rather than the real /proc mount)
 * as a defensive "is this really an unrestricted procfs" sanity check
 * before ever touching uid_map/gid_map, independent of whether the
 * container itself asked for a new user namespace. When the file does not
 * exist at all, runc aborts container creation with "unsafe procfs
 * detected ... proc/self/setgroups: no such file or directory", which is
 * exactly the failure this file exists to avoid.
 *
 * Unlike arbitrary uid_map/gid_map parsing (deliberately left unimplemented
 * -- see shadow_ns/README.md -- because it would require reconstructing
 * real per-namespace id mappings), "setgroups" needs no such state: real
 * setgroups(7) semantics are just a one-way "allow" -> "deny" latch that
 * gates whether a task without CAP_SETGID may still write its own
 * uid_map/gid_map. Faking that latch's read/write behaviour on a synthetic,
 * anonymous file handed back in place of the missing dentry is enough to
 * satisfy every caller observed in the wild, without reimplementing any
 * proc_pid_lookup()/tgid_base_stuff[] internals.
 *
 * This intercepts the raw open()/openat()/openat2() syscalls (the same
 * pattern shadow_ns already uses for getuid()/unshare()/...): call through
 * to the real syscall first, and only fabricate a descriptor when the real
 * open genuinely failed with -ENOENT *and* the requested path is
 * unambiguously a "setgroups" leaf under a procfs-rooted pid directory
 * (".../<pid|self|thread-self>/setgroups", or a bare "setgroups" opened
 * relative to a dfd whose superblock is procfs -- the shape a detached
 * fsopen("proc")+fsmount() dirfd takes). Any other -ENOENT is returned
 * untouched.
 *
 * The fabricated descriptor must also survive being "reopened": several
 * hardened procfs helpers (e.g. runc/containerd's securejoin/pathrs-lite
 * ReopenFd()) always re-open a just-opened procfs fd a second time through
 * the "/proc/thread-self/fd/<n>" magic link, as a defensive check against
 * symlink races -- independent of whether the original open used O_PATH.
 * A descriptor created via the simpler anon_inode_getfd() API fails that
 * reopen with -ENXIO ("no such device or address"): its backing inode is
 * the single, shared anon_inode_inode, whose ->i_fop is never touched by
 * anon_inode_getfd() and so remains fs/inode.c's default no_open_fops
 * (->open() == no_open(), unconditionally -ENXIO). shadow_ns_procfs.c
 * therefore uses anon_inode_getfile_secure() instead, which allocates a
 * *private* inode per fd, and explicitly points that inode's ->i_fop at
 * shadow_setgroups_fops so the magic-link reopen succeeds.
 */
#include "shadow_ns_internal.h"

#include <linux/anon_inodes.h>
#include <linux/ctype.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/magic.h>

/*
 * The allow/deny latch lives on the *inode*, not in a per-file heap
 * allocation: runc/containerd's "reopen through /proc/thread-self/fd/<n>"
 * safety check (see below) ends up creating a second struct file that backs
 * onto the very same inode, and both files need to observe the same latch
 * state. Storing it in file->private_data instead would require either
 * sharing/refcounting that allocation across files (extra complexity for no
 * benefit) or risking a double free from two independent release() calls.
 * i_private needs no allocation at all: NULL means "allow", non-NULL means
 * "deny" -- exactly the one-way latch semantics of real setgroups(7).
 */
static ssize_t shadow_setgroups_read(struct file *file, char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	bool deny = !!file_inode(file)->i_private;
	const char *str = deny ? "deny\n" : "allow\n";

	return simple_read_from_buffer(ubuf, count, ppos, str, strlen(str));
}

static ssize_t shadow_setgroups_write(struct file *file,
				       const char __user *ubuf, size_t count,
				       loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	char kbuf[8];
	size_t n = min(count, sizeof(kbuf) - 1);

	if (copy_from_user(kbuf, ubuf, n))
		return -EFAULT;
	kbuf[n] = '\0';
	if (n && kbuf[n - 1] == '\n')
		kbuf[n - 1] = '\0';

	/*
	 * Real setgroups(7): "allow" is only a no-op re-affirmation of the
	 * default, "deny" latches permanently (a later "allow" is rejected
	 * once denied).  No other value is accepted.
	 */
	if (!strcmp(kbuf, "deny")) {
		inode->i_private = (void *)1UL;
	} else if (strcmp(kbuf, "allow") || inode->i_private) {
		return -EINVAL;
	}

	*ppos += count;
	return count;
}

/*
 * Only ever invoked when this inode is opened a *second* time, through the
 * "/proc/thread-self/fd/<n>" magic-link reopen every modern
 * runc/containerd performs on a freshly-opened procfs fd (see
 * securejoin/pathrs-lite's ReopenFd()) -- see shadow_ns_setgroups_create_fd()
 * for why this callback needs to exist at all. Nothing to set up: the latch
 * state already lives on the inode, and reads/writes reach it directly via
 * file_inode(), independent of file->private_data.
 */
static int shadow_setgroups_open(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations shadow_setgroups_fops = {
	.owner		= THIS_MODULE,
	.open		= shadow_setgroups_open,
	.read		= shadow_setgroups_read,
	.write		= shadow_setgroups_write,
	.llseek		= default_llseek,
};

static long shadow_ns_setgroups_create_fd(void)
{
	struct file *file;
	int fd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	/*
	 * anon_inode_getfd()'s shared, singleton anon_inode_inode cannot be
	 * used here: its default file_operations (fs/inode.c's
	 * no_open_fops, installed by inode_init_always() and never
	 * overridden by alloc_anon_inode()) has ->open() == no_open(),
	 * which unconditionally returns -ENXIO. That default is invisible
	 * for a "normal" anon_inode fd (nothing ever opens it a second
	 * time), but modern runc/containerd always "reopen" a just-opened
	 * procfs fd through the "/proc/thread-self/fd/<n>" magic link as a
	 * defensive safety check (see pathrs-lite's ReopenFd()) -- and that
	 * reopen is a genuine VFS open() that goes through the inode's own
	 * ->i_fop, not the fabricated file's ->f_op. Against the shared
	 * singleton inode this reopen would fail every caller in the kernel
	 * with "no such device or address", so anon_inode_getfile_secure()
	 * is used instead to get a private, per-fd inode whose ->i_fop can
	 * safely be pointed at shadow_setgroups_fops without affecting any
	 * other anon-inode-backed fd on the system.
	 */
	file = anon_inode_getfile_secure("[shadow_setgroups]",
					  &shadow_setgroups_fops, NULL,
					  O_RDWR | O_CLOEXEC, NULL);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		return PTR_ERR(file);
	}

	file_inode(file)->i_fop = &shadow_setgroups_fops;

	fd_install(fd, file);
	return fd;
}

/* "self", "thread-self" or an all-digits pid directory name. */
static bool shadow_ns_component_is_pid_dir(const char *s)
{
	if (!s || !*s)
		return false;
	if (!strcmp(s, "self") || !strcmp(s, "thread-self"))
		return true;
	for (; *s; s++) {
		if (!isdigit((unsigned char)*s))
			return false;
	}
	return true;
}

/*
 * Bare "setgroups" with no directory component in the path string only
 * makes sense relative to a dfd that is itself rooted somewhere inside a
 * procfs mount (e.g. a detached fsopen("proc")+fsmount() dirfd handed
 * straight to openat2(), or an already-open /proc/<pid> directory fd).
 */
static bool shadow_ns_dfd_is_procfs(int dfd)
{
	struct fd f;
	bool ret;

	if (dfd == AT_FDCWD)
		return false;

	f = fdget(dfd);
	if (!f.file)
		return false;
	ret = f.file->f_path.dentry->d_sb->s_magic == PROC_SUPER_MAGIC;
	fdput(f);
	return ret;
}

static bool shadow_ns_path_wants_setgroups(int dfd, const char __user *upath)
{
	char buf[192];
	long n;
	char *slash, *base, *dir_last;

	if (!upath)
		return false;

	n = strncpy_from_user(buf, upath, sizeof(buf));
	if (n <= 0 || n >= sizeof(buf))
		return false;

	slash = strrchr(buf, '/');
	base = slash ? slash + 1 : buf;
	if (strcmp(base, "setgroups"))
		return false;

	if (!slash)
		return shadow_ns_dfd_is_procfs(dfd);

	*slash = '\0';
	dir_last = strrchr(buf, '/');
	dir_last = dir_last ? dir_last + 1 : buf;
	return shadow_ns_component_is_pid_dir(dir_last);
}

static long (*real_sys_openat2)(const struct pt_regs *regs);
static long (*real_sys_openat)(const struct pt_regs *regs);
static long (*real_sys_open)(const struct pt_regs *regs);

static long shadow_ns_hook_openat2(const struct pt_regs *regs)
{
	int dfd = (int)shadow_ns_sys_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg1(regs);
	long ret = real_sys_openat2(regs);

	if (ret != -ENOENT || !shadow_ns_path_wants_setgroups(dfd, upath))
		return ret;
	return shadow_ns_setgroups_create_fd();
}

static long shadow_ns_hook_openat(const struct pt_regs *regs)
{
	int dfd = (int)shadow_ns_sys_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg1(regs);
	long ret = real_sys_openat(regs);

	if (ret != -ENOENT || !shadow_ns_path_wants_setgroups(dfd, upath))
		return ret;
	return shadow_ns_setgroups_create_fd();
}

static long shadow_ns_hook_open(const struct pt_regs *regs)
{
	const char __user *upath =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	long ret = real_sys_open(regs);

	if (ret != -ENOENT || !shadow_ns_path_wants_setgroups(AT_FDCWD, upath))
		return ret;
	return shadow_ns_setgroups_create_fd();
}

static const char * const shadow_ns_openat2_names[] = {
	"__arm64_sys_openat2", "__x64_sys_openat2", "sys_openat2", NULL,
};
static const char * const shadow_ns_openat_names[] = {
	"__arm64_sys_openat", "__x64_sys_openat", "sys_openat", NULL,
};
static const char * const shadow_ns_open_names[] = {
	"__arm64_sys_open", "__x64_sys_open", "sys_open", NULL,
};

static struct shadow_hook shadow_ns_openat2_hook =
	SHADOW_HOOK(shadow_ns_openat2_names, shadow_ns_hook_openat2,
		    &real_sys_openat2);
static struct shadow_hook shadow_ns_openat_hook =
	SHADOW_HOOK(shadow_ns_openat_names, shadow_ns_hook_openat,
		    &real_sys_openat);
static struct shadow_hook shadow_ns_open_hook =
	SHADOW_HOOK(shadow_ns_open_names, shadow_ns_hook_open,
		    &real_sys_open);

struct shadow_hook *shadow_ns_procfs_hooks[] = {
	&shadow_ns_openat2_hook,
	&shadow_ns_openat_hook,
	&shadow_ns_open_hook,
	NULL,
};
