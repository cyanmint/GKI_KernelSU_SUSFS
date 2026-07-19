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
 */
#include "shadow_ns_internal.h"

#include <linux/anon_inodes.h>
#include <linux/ctype.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/magic.h>

struct shadow_setgroups_file {
	bool deny;
};

static ssize_t shadow_setgroups_read(struct file *file, char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct shadow_setgroups_file *sf = file->private_data;
	const char *str = sf->deny ? "deny\n" : "allow\n";

	return simple_read_from_buffer(ubuf, count, ppos, str, strlen(str));
}

static ssize_t shadow_setgroups_write(struct file *file,
				       const char __user *ubuf, size_t count,
				       loff_t *ppos)
{
	struct shadow_setgroups_file *sf = file->private_data;
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
	 * once denied). No other value is accepted.
	 */
	if (!strcmp(kbuf, "deny")) {
		sf->deny = true;
	} else if (strcmp(kbuf, "allow") || sf->deny) {
		return -EINVAL;
	}

	*ppos += count;
	return count;
}

static int shadow_setgroups_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static const struct file_operations shadow_setgroups_fops = {
	.owner		= THIS_MODULE,
	.read		= shadow_setgroups_read,
	.write		= shadow_setgroups_write,
	.release	= shadow_setgroups_release,
	.llseek		= default_llseek,
};

static long shadow_ns_setgroups_create_fd(void)
{
	struct shadow_setgroups_file *sf;
	int fd;

	sf = kzalloc(sizeof(*sf), GFP_KERNEL);
	if (!sf)
		return -ENOMEM;

	fd = anon_inode_getfd("[shadow_setgroups]", &shadow_setgroups_fops,
			      sf, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		kfree(sf);
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
