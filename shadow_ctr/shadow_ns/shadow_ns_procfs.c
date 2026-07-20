// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ns_procfs - fabricate /proc/<pid>/setgroups on kernels genuinely
 * missing CONFIG_USER_NS, and isolate /proc for shadow_ns's simulated PID
 * namespace (CONFIG_PID_NS genuinely absent).
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
 * therefore uses anon_inode_getfd_secure() instead, which allocates a
 * *private* inode per fd, and explicitly points that inode's ->i_fop at
 * shadow_setgroups_fops so the magic-link reopen succeeds.
 *
 * This file also gives shadow_ns's simulated PID namespace real /proc
 * isolation (see shadow_ns_proc_open()/shadow_ns_hook_getdents64() below):
 * without it, a task moved into a shadow PID namespace still sees every
 * host pid under /proc (openat("/proc/<real-host-pid>/...") succeeds and
 * `ls /proc`/`readdir()` lists them), which is exactly the "pid ns isn't
 * really isolated" bug reported against this module. Real
 * kernel/pid_namespace.c gets this for free because fs/proc/base.c and
 * fs/proc/root.c consult task_active_pid_ns() directly; a simulated
 * namespace has no such kernel-side hook, so shadow_ns instead translates
 * /proc/<vpid> paths to their real /proc/<rpid> equivalent (or -ENOENT for
 * a real pid that isn't a member) on open, and filters+renames /proc's own
 * root directory listing on getdents64, exactly mirroring the vpid<->rpid
 * translation shadow_ns_pid.c already performs for getpid()/kill()/wait4().
 */
#include "shadow_ns_internal.h"

#include <linux/anon_inodes.h>
#include <linux/ctype.h>
#include <linux/dirent.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "shadow_ctr_compat.h"

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

typedef int (*shadow_anon_inode_getfd_secure_fn)(const char *,
						  const struct file_operations *,
						  void *, int,
						  const struct inode *);

static long shadow_ns_setgroups_create_fd(void)
{
	shadow_anon_inode_getfd_secure_fn anon_inode_getfd_secure_fn;
	struct file *file;
	int fd;

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
	 * with "no such device or address", so anon_inode_getfd_secure() is
	 * used instead to get a private, per-fd inode whose ->i_fop can
	 * safely be pointed at shadow_setgroups_fops without affecting any
	 * other anon-inode-backed fd on the system.
	 *
	 * NOTE: anon_inode_getfile_secure() (the struct-file-returning
	 * sibling of this call, which would avoid the fget()/fput() below)
	 * is deliberately *not* used: unlike anon_inode_getfd()/
	 * anon_inode_getfd_secure()/anon_inode_getfile(), it has no
	 * EXPORT_SYMBOL(_GPL) in fs/anon_inodes.c, so referencing it here
	 * makes the whole module fail to insmod with "Unknown symbol
	 * anon_inode_getfile_secure".
	 *
	 * anon_inode_getfd_secure() itself *is* EXPORT_SYMBOL_GPL()'d in
	 * fs/anon_inodes.c, but production GKI kernels built with
	 * CONFIG_TRIM_UNUSED_KSYMS strip the export entirely because no
	 * built-in code calls it -- the symbol still exists as ordinary text
	 * in /proc/kallsyms, but a direct call from an out-of-tree module
	 * makes the whole shadow_ctr.ko fail to insmod with "Unknown symbol
	 * anon_inode_getfd_secure" (same class of failure previously hit
	 * with path_put() in shadow_mqueue). Resolve it via
	 * shadow_hook_resolve() (kprobe-based kallsyms lookup) instead of
	 * calling it directly, exactly like shadow_mqueue does for
	 * kern_path/vfs_mkdir/path_mount/path_put.
	 */
	anon_inode_getfd_secure_fn = (shadow_anon_inode_getfd_secure_fn)
		shadow_hook_resolve("anon_inode_getfd_secure");
	if (!anon_inode_getfd_secure_fn)
		return -ENOENT;

	fd = anon_inode_getfd_secure_fn("[shadow_setgroups]",
					 &shadow_setgroups_fops, NULL,
					 O_RDWR | O_CLOEXEC, NULL);
	if (fd < 0)
		return fd;

	file = fget(fd);
	if (file) {
		file_inode(file)->i_fop = &shadow_setgroups_fops;
		fput(file);
	}

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

/* An all-digits, non-empty component of exactly `len` bytes at `s`. */
static bool shadow_ns_component_is_digits(const char *s, size_t len)
{
	size_t i;

	if (!len)
		return false;
	for (i = 0; i < len; i++) {
		if (!isdigit((unsigned char)s[i]))
			return false;
	}
	return true;
}

/*
 * Bare "setgroups"/numeric-pid path components with no directory prefix
 * only make sense relative to a dfd that is itself rooted somewhere inside
 * a procfs mount (e.g. a detached fsopen("proc")+fsmount() dirfd handed
 * straight to openat2(), or an already-open /proc/<pid> directory fd).
 */
static bool shadow_ns_dfd_is_procfs(int dfd)
{
	struct fd f;
	bool ret;

	if (dfd == AT_FDCWD)
		return false;

	f = fdget(dfd);
	if (fd_empty(f))
		return false;
	ret = fd_file(f)->f_path.dentry->d_sb->s_magic == PROC_SUPER_MAGIC;
	fdput(f);
	return ret;
}

/*
 * proc_ns vendoring: fabricate /proc/<pid>/ns/pid and
 * /proc/<pid>/ns/pid_for_children on a kernel genuinely missing
 * CONFIG_PID_NS, the same way fs/nsfs.c + fs/proc/namespaces.c expose the
 * real thing when it is present.
 *
 * Ported from (names kept identical for cross-reference):
 *   - kernel/pid_namespace.c: pidns_operations.name = "pid",
 *     pidns_for_children_operations.name = "pid_for_children" -- both the
 *     dentry name under .../ns/ *and* the "%s:[<ino>]" readlink prefix.
 *   - fs/nsfs.c:ns_dname()/ns_get_name(): "%s:[%lu]" format, ns_ops->name
 *     plus the namespace's inode number. shadow_ns has no real inode; it
 *     uses the namespace's own shadow_ns_map id instead (see shadow_ns_base.c
 *     -- globally unique across every shadow_ns type/instance, exactly like
 *     a real ns_common.inum), so the number is self-consistent between the
 *     symlink target text below and the fd shadow_ns_hook_setns() ends up
 *     resolving back to the same shadow_ns id.
 *   - fs/proc/namespaces.c:proc_ns_dir_readdir(): the two synthetic
 *     directory entries added to a real /proc/<pid>/ns listing.
 *
 * The fabricated fd itself does not hold a shadow_ns reference: it only
 * stashes the plain shadow_ns id (an integer, not a pointer) in its private
 * inode's ->i_private, exactly like shadow_setgroups_fops's allow/deny latch
 * above. shadow_ns_procfs_nsfd_to_id() (used by shadow_ns_core.c's setns(2)
 * fallback) re-resolves that id through shadow_ns_get() at the point it is
 * actually used, so a fd outliving its shadow_ns (e.g. every member of the
 * namespace has since exited) safely yields "namespace not found" instead
 * of a dangling pointer.
 */
#define SHADOW_NS_NSFD_NAME_PID		"pid"
#define SHADOW_NS_NSFD_NAME_PID_CHILD	"pid_for_children"

static int shadow_ns_nsfd_open(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations shadow_ns_nsfd_fops = {
	.owner		= THIS_MODULE,
	.open		= shadow_ns_nsfd_open,
	.llseek		= NULL,
};

static long shadow_ns_nsfd_create_fd(struct shadow_ns *ns)
{
	shadow_anon_inode_getfd_secure_fn anon_inode_getfd_secure_fn;
	struct file *file;
	int fd;

	anon_inode_getfd_secure_fn = (shadow_anon_inode_getfd_secure_fn)
		shadow_hook_resolve("anon_inode_getfd_secure");
	if (!anon_inode_getfd_secure_fn)
		return -ENOENT;

	fd = anon_inode_getfd_secure_fn("[shadow_pid_ns]", &shadow_ns_nsfd_fops,
					 NULL, O_RDONLY | O_CLOEXEC, NULL);
	if (fd < 0)
		return fd;

	file = fget(fd);
	if (file) {
		file_inode(file)->i_fop = &shadow_ns_nsfd_fops;
		file_inode(file)->i_private = (void *)(unsigned long)ns->id;
		fput(file);
	}

	return fd;
}

u32 shadow_ns_procfs_nsfd_to_id(int fd)
{
	struct fd f;
	u32 id = 0;

	f = fdget(fd);
	if (fd_empty(f))
		return 0;
	if (fd_file(f)->f_op == &shadow_ns_nsfd_fops)
		id = (u32)(unsigned long)file_inode(fd_file(f))->i_private;
	fdput(f);
	return id;
}

/*
 * shadow_ns_resolve_piddir_rpid() - resolve a "self"/"thread-self"/numeric
 * path component (as found immediately under /proc/) to the real (host)
 * pid it names. A numeric component is first tried as a vpid in the
 * caller's own active simulated PID namespace (mirroring the translation
 * shadow_ns_proc_open() already does for a plain /proc/<vpid>/... open);
 * if that lookup fails (no active namespace, or the number isn't a member),
 * it is taken to already be a real pid.
 */
static pid_t shadow_ns_resolve_piddir_rpid(const char *comp)
{
	struct shadow_ns *ns;
	long val;
	pid_t rpid;

	if (!strcmp(comp, "self"))
		return task_tgid_nr(current);
	if (!strcmp(comp, "thread-self"))
		return task_pid_nr(current);
	if (kstrtol(comp, 10, &val) || val <= 0 || val > INT_MAX)
		return 0;

	ns = shadow_ns_current_pidns();
	if (!ns)
		return (pid_t)val;
	rpid = shadow_ns_pidns_to_rpid(ns->pid, (pid_t)val);
	shadow_ns_put(ns);
	return rpid ? rpid : (pid_t)val;
}

/*
 * shadow_ns_path_ns_entry() - does @upath (relative to @dfd) name
 * ".../<piddir>/ns/pid" or ".../<piddir>/ns/pid_for_children"? If so,
 * returns the matched entry name (a pointer to one of the two string
 * literals above) and resolves the owning task's real pid into *rpid.
 * Returns NULL otherwise (including on any parse failure -- callers must
 * fall back to the real syscall unchanged).
 */
static const char *shadow_ns_path_ns_entry(int dfd, const char __user *upath,
					    pid_t *rpid)
{
	char buf[192];
	char *base, *slash1, *slash2, *piddir;
	long n;

	if (!upath)
		return NULL;

	n = strncpy_from_user(buf, upath, sizeof(buf));
	if (n <= 0 || n >= sizeof(buf))
		return NULL;

	if (buf[0] == '/') {
		if (strncmp(buf, "/proc/", 6))
			return NULL;
		base = buf + 6;
	} else {
		if (dfd == AT_FDCWD || !shadow_ns_dfd_is_procfs(dfd))
			return NULL;
		base = buf;
	}

	slash1 = strchr(base, '/');
	if (!slash1)
		return NULL;
	*slash1 = '\0';
	piddir = base;
	if (!shadow_ns_component_is_pid_dir(piddir))
		return NULL;

	slash1++;
	if (strncmp(slash1, "ns/", 3))
		return NULL;
	slash2 = slash1 + 3;

	if (!strcmp(slash2, SHADOW_NS_NSFD_NAME_PID)) {
		*rpid = shadow_ns_resolve_piddir_rpid(piddir);
		return *rpid > 0 ? SHADOW_NS_NSFD_NAME_PID : NULL;
	}
	if (!strcmp(slash2, SHADOW_NS_NSFD_NAME_PID_CHILD)) {
		*rpid = shadow_ns_resolve_piddir_rpid(piddir);
		return *rpid > 0 ? SHADOW_NS_NSFD_NAME_PID_CHILD : NULL;
	}
	return NULL;
}

/*
 * shadow_ns_ns_entry_create_fd() - the open()/openat()/openat2() fallback:
 * called only after the real syscall has already failed with -ENOENT for a
 * path shadow_ns_path_ns_entry() recognized.
 */
static long shadow_ns_ns_entry_create_fd(const char *entry, pid_t rpid)
{
	struct shadow_ns *ns;
	long fd;

	ns = shadow_ns_pidns_for_tgid(rpid, !strcmp(entry, SHADOW_NS_NSFD_NAME_PID_CHILD));
	if (!ns)
		return -ENOENT;

	fd = shadow_ns_nsfd_create_fd(ns);
	shadow_ns_put(ns);
	return fd;
}

/*
 * shadow_ns_ns_entry_readlink() - fabricate the "pid:[<id>]"/
 * "pid_for_children:[<id>]" symlink target text real readlink(2) on
 * .../ns/pid{,_for_children} would return, for a caller whose @dfd/@upath
 * shadow_ns_path_ns_entry() recognizes. Returns the string length copied
 * (>= 0) on success, or a negative errno; callers must only use this after
 * confirming shadow_ns_path_ns_entry() matched.
 */
static long shadow_ns_ns_entry_readlink(const char *entry, pid_t rpid,
					 char __user *ubuf, int bufsiz)
{
	struct shadow_ns *ns;
	char name[64];
	int n;

	ns = shadow_ns_pidns_for_tgid(rpid, !strcmp(entry, SHADOW_NS_NSFD_NAME_PID_CHILD));
	if (!ns)
		return -ENOENT;

	n = snprintf(name, sizeof(name), "%s:[%u]", entry, ns->id);
	shadow_ns_put(ns);
	if (n < 0)
		return -ENOENT;

	if (bufsiz > n)
		bufsiz = n;
	if (copy_to_user(ubuf, name, bufsiz))
		return -EFAULT;
	return bufsiz;
}


typedef struct file *(*shadow_filp_open_fn)(const char *, int, umode_t);
typedef struct file *(*shadow_file_open_root_fn)(const struct path *,
						  const char *, int, umode_t);

/*
 * shadow_ns_proc_open() - open a "/proc/<vpid>[/...]" (absolute) or, when
 * `dfd` is itself an already-open procfs directory fd, a relative
 * "<vpid>[/...]" path, translating the leading numeric pid component from
 * the caller's simulated-PID-namespace-local vpid to the real host rpid
 * before actually resolving it.
 *
 * Returns:
 *   LONG_MIN - not applicable (no active simulated PID namespace, or the
 *              path isn't a numeric /proc/<pid> access at all); caller must
 *              fall back to the real syscall unchanged.
 *   -ENOENT  - the requested vpid is not a registered member of the
 *              caller's simulated namespace, so it must not be visible --
 *              exactly what real PID namespace /proc isolation does for a
 *              pid outside the namespace.
 *   >= 0     - a freshly installed fd for the translated path.
 *   < 0      - some other real error translating/opening the path.
 */
static long shadow_ns_proc_open(int dfd, const char __user *upath, int flags,
				 umode_t mode)
{
	struct shadow_ns *ns;
	struct fd dirfd;
	bool have_dirfd = false;
	char buf[192];
	char tail[192];
	char full[224];
	const char *path, *p, *slash, *rest;
	size_t len;
	char pidbuf[12];
	long vpid;
	pid_t rpid;
	shadow_filp_open_fn filp_open_fn;
	shadow_file_open_root_fn file_open_root_fn;
	struct file *file;
	int fd, n;

	if (!upath)
		return LONG_MIN;

	n = strncpy_from_user(buf, upath, sizeof(buf));
	if (n <= 0 || (size_t)n >= sizeof(buf))
		return LONG_MIN;

	if (buf[0] == '/') {
		if (strncmp(buf, "/proc/", 6))
			return LONG_MIN;
		path = buf + 6;
	} else {
		if (dfd == AT_FDCWD || !shadow_ns_dfd_is_procfs(dfd))
			return LONG_MIN;
		path = buf;
	}

	p = path;
	slash = strchr(p, '/');
	len = slash ? (size_t)(slash - p) : strlen(p);
	if (!len || len >= sizeof(pidbuf) ||
	    !shadow_ns_component_is_digits(p, len))
		return LONG_MIN;

	ns = shadow_ns_current_pidns();
	if (!ns)
		return LONG_MIN;

	memcpy(pidbuf, p, len);
	pidbuf[len] = '\0';
	if (kstrtol(pidbuf, 10, &vpid) || vpid <= 0 || vpid > INT_MAX) {
		shadow_ns_put(ns);
		return LONG_MIN;
	}

	rpid = shadow_ns_pidns_to_rpid(ns->pid, (pid_t)vpid);
	shadow_ns_put(ns);
	if (!rpid)
		return -ENOENT;

	rest = slash ? slash : "";
	n = snprintf(tail, sizeof(tail), "%d%s", rpid, rest);
	if (n < 0 || (size_t)n >= sizeof(tail))
		return -ENOENT;

	if (buf[0] == '/') {
		n = snprintf(full, sizeof(full), "/proc/%s", tail);
		if (n < 0 || (size_t)n >= sizeof(full))
			return -ENOENT;

		filp_open_fn = (shadow_filp_open_fn)
			shadow_hook_resolve("filp_open");
		if (!filp_open_fn)
			return LONG_MIN;
		file = filp_open_fn(full, flags, mode);
	} else {
		dirfd = fdget(dfd);
		if (fd_empty(dirfd))
			return LONG_MIN;
		have_dirfd = true;

		file_open_root_fn = (shadow_file_open_root_fn)
			shadow_hook_resolve("file_open_root");
		if (!file_open_root_fn) {
			fdput(dirfd);
			return LONG_MIN;
		}
		file = file_open_root_fn(&fd_file(dirfd)->f_path, tail, flags,
					  mode);
	}
	if (have_dirfd)
		fdput(dirfd);

	if (IS_ERR(file))
		return PTR_ERR(file);

	fd = get_unused_fd_flags(flags);
	if (fd < 0) {
		filp_close(file, NULL);
		return fd;
	}
	fd_install(fd, file);
	return fd;
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

/*
 * shadow_ns_open_fallback() - shared -ENOENT fallback for openat2/openat/
 * open: only reached once the real syscall has already failed to open the
 * path. Tries, in order, the fabricated "setgroups" leaf and the fabricated
 * ".../ns/pid"|".../ns/pid_for_children" leaves; returns @ret unchanged if
 * neither matches.
 */
static long shadow_ns_open_fallback(int dfd, const char __user *upath, long ret)
{
	const char *entry;
	pid_t rpid;

	if (ret != -ENOENT)
		return ret;

	if (shadow_ns_path_wants_setgroups(dfd, upath))
		return shadow_ns_setgroups_create_fd();

	entry = shadow_ns_path_ns_entry(dfd, upath, &rpid);
	if (entry)
		return shadow_ns_ns_entry_create_fd(entry, rpid);

	return ret;
}

static long shadow_ns_hook_openat2(const struct pt_regs *regs)
{
	int dfd = (int)shadow_ns_sys_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg1(regs);
	const void __user *uhow =
		(const void __user *)(uintptr_t)shadow_ns_sys_arg2(regs);
	/*
	 * struct open_how { u64 flags; u64 mode; u64 resolve; } (uapi
	 * linux/openat2.h) -- only flags/mode are needed here (mode never
	 * matters for a /proc read, since translated opens never O_CREAT);
	 * `resolve` is intentionally left unexamined.
	 */
	struct {
		u64 flags;
		u64 mode;
	} how = { .flags = O_RDONLY };
	long ret;

	if (uhow && copy_from_user(&how, uhow, sizeof(how)))
		how.flags = O_RDONLY;

	ret = shadow_ns_proc_open(dfd, upath, (int)how.flags,
				   (umode_t)how.mode);
	if (ret != LONG_MIN)
		return ret;

	ret = real_sys_openat2(regs);
	return shadow_ns_open_fallback(dfd, upath, ret);
}

static long shadow_ns_hook_openat(const struct pt_regs *regs)
{
	int dfd = (int)shadow_ns_sys_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg1(regs);
	int flags = (int)shadow_ns_sys_arg2(regs);
	long ret = shadow_ns_proc_open(dfd, upath, flags, 0);

	if (ret != LONG_MIN)
		return ret;

	ret = real_sys_openat(regs);
	return shadow_ns_open_fallback(dfd, upath, ret);
}

static long shadow_ns_hook_open(const struct pt_regs *regs)
{
	const char __user *upath =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	int flags = (int)shadow_ns_sys_arg1(regs);
	long ret = shadow_ns_proc_open(AT_FDCWD, upath, flags, 0);

	if (ret != LONG_MIN)
		return ret;

	ret = real_sys_open(regs);
	return shadow_ns_open_fallback(AT_FDCWD, upath, ret);
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

/*
 * shadow_ns_hook_readlinkat()/shadow_ns_hook_readlink() - fabricate the
 * "pid:[<id>]"/"pid_for_children:[<id>]" symlink target text for
 * .../ns/pid{,_for_children}, mirroring fs/proc/namespaces.c's
 * proc_ns_readlink(). The real readlink(2) fails with -ENOENT for these
 * paths (the dentries don't exist without CONFIG_PID_NS), so -- exactly
 * like the open() fallback above -- the fabrication only kicks in once the
 * real syscall has already failed.
 */
static long (*real_sys_readlinkat)(const struct pt_regs *regs);
static long (*real_sys_readlink)(const struct pt_regs *regs);

static long shadow_ns_hook_readlinkat(const struct pt_regs *regs)
{
	int dfd = (int)shadow_ns_sys_arg0(regs);
	const char __user *upath =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg1(regs);
	char __user *ubuf =
		(char __user *)(uintptr_t)shadow_ns_sys_arg2(regs);
	long ret = real_sys_readlinkat(regs);
	const char *entry;
	pid_t rpid;

	if (ret != -ENOENT)
		return ret;

	entry = shadow_ns_path_ns_entry(dfd, upath, &rpid);
	if (!entry)
		return ret;

	/*
	 * bufsiz (4th syscall arg) doesn't fit shadow_ns_sys_arg2()'s
	 * three-argument helper set; read it directly off the same
	 * arch-specific register readlinkat(2) passes it in.
	 */
#if defined(CONFIG_ARM64)
	return shadow_ns_ns_entry_readlink(entry, rpid, ubuf,
					    (int)regs->regs[3]);
#elif defined(CONFIG_X86_64)
	return shadow_ns_ns_entry_readlink(entry, rpid, ubuf, (int)regs->r10);
#endif
}

static long shadow_ns_hook_readlink(const struct pt_regs *regs)
{
	const char __user *upath =
		(const char __user *)(uintptr_t)shadow_ns_sys_arg0(regs);
	char __user *ubuf =
		(char __user *)(uintptr_t)shadow_ns_sys_arg1(regs);
	int bufsiz = (int)shadow_ns_sys_arg2(regs);
	long ret = real_sys_readlink(regs);
	const char *entry;
	pid_t rpid;

	if (ret != -ENOENT)
		return ret;

	entry = shadow_ns_path_ns_entry(AT_FDCWD, upath, &rpid);
	if (!entry)
		return ret;

	return shadow_ns_ns_entry_readlink(entry, rpid, ubuf, bufsiz);
}

static const char * const shadow_ns_readlinkat_names[] = {
	"__arm64_sys_readlinkat", "__x64_sys_readlinkat", "sys_readlinkat",
	NULL,
};
static const char * const shadow_ns_readlink_names[] = {
	"__arm64_sys_readlink", "__x64_sys_readlink", "sys_readlink", NULL,
};

static struct shadow_hook shadow_ns_readlinkat_hook =
	SHADOW_HOOK(shadow_ns_readlinkat_names, shadow_ns_hook_readlinkat,
		    &real_sys_readlinkat);
static struct shadow_hook shadow_ns_readlink_hook =
	SHADOW_HOOK(shadow_ns_readlink_names, shadow_ns_hook_readlink,
		    &real_sys_readlink);

/*
 * shadow_ns_hook_getdents64() - filter+rename /proc's own root directory
 * listing for a task with an active simulated PID namespace: real host pids
 * that are not registered members of the namespace are dropped entirely
 * (mirroring the -ENOENT a direct /proc/<rpid> open gets from
 * shadow_ns_proc_open() above), and member entries are renamed from their
 * real rpid to the namespace-local vpid getpid()/kill()/wait4() already
 * report.
 *
 * Deliberately scoped to the /proc root directory only: subdirectory
 * listings such as /proc/<pid>/task/ (thread ids) are left untouched, same
 * documented-limitation tradeoff as the rest of shadow_ns's PID simulation
 * (see README.md).
 */
static long (*real_sys_getdents64)(const struct pt_regs *regs);

static bool shadow_ns_fd_is_proc_root(int fd)
{
	struct fd f;
	bool ret;

	f = fdget(fd);
	if (fd_empty(f))
		return false;
	ret = fd_file(f)->f_path.dentry->d_sb->s_magic == PROC_SUPER_MAGIC &&
	      fd_file(f)->f_path.dentry == fd_file(f)->f_path.dentry->d_sb->s_root;
	fdput(f);
	return ret;
}

/*
 * shadow_ns_fd_is_proc_ns_dir() - is @fd an already-open "/proc/<pid>/ns"
 * directory (any pid, self/thread-self/vpid/rpid alike)? If so, resolves
 * the owning task's real pid into *rpid_out, exactly like
 * shadow_ns_path_ns_entry() does from a path string -- this variant works
 * off the already-open directory fd's dentry instead, for
 * shadow_ns_hook_getdents64()'s benefit (readdir(3) always opendir()s the
 * directory once and getdents64()s the resulting fd repeatedly, so there is
 * no path string available at this point, only the fd).
 */
static bool shadow_ns_fd_is_proc_ns_dir(int fd, pid_t *rpid_out)
{
	struct fd f;
	struct dentry *dentry, *parent;
	char comp[16];
	size_t len;
	bool ret = false;

	f = fdget(fd);
	if (fd_empty(f))
		return false;

	dentry = fd_file(f)->f_path.dentry;
	if (dentry->d_sb->s_magic != PROC_SUPER_MAGIC)
		goto out;
	if (strcmp((const char *)dentry->d_name.name, "ns"))
		goto out;

	parent = dentry->d_parent;
	len = min_t(size_t, parent->d_name.len, sizeof(comp) - 1);
	memcpy(comp, parent->d_name.name, len);
	comp[len] = '\0';
	if (!shadow_ns_component_is_pid_dir(comp))
		goto out;

	*rpid_out = shadow_ns_resolve_piddir_rpid(comp);
	ret = *rpid_out > 0;
out:
	fdput(f);
	return ret;
}

/*
 * shadow_ns_getdents64_append_ns_entries() - append synthetic "pid"/
 * "pid_for_children" dirents to an already-fetched real
 * /proc/<pid>/ns listing, mirroring
 * fs/proc/namespaces.c:proc_ns_dir_readdir()'s two extra entries. Only
 * ever called once shadow_ns_fd_is_proc_ns_dir() has confirmed @fd is a
 * "ns" directory and resolved its owning real pid into @rpid.
 */
static long shadow_ns_getdents64_append_ns_entries(void __user *udirp,
						    unsigned int count,
						    long ret, pid_t rpid)
{
	struct shadow_ns *ns_pid, *ns_children;
	char kbuf[256];
	long extra_len = 0;

	ns_pid = shadow_ns_pidns_for_tgid(rpid, false);
	ns_children = shadow_ns_pidns_for_tgid(rpid, true);
	if (!ns_pid && !ns_children)
		return ret;

	if (ns_pid) {
		struct linux_dirent64 *d = (struct linux_dirent64 *)kbuf;
		int n = strlen(SHADOW_NS_NSFD_NAME_PID);
		unsigned short reclen = ALIGN(
			offsetof(struct linux_dirent64, d_name) + n + 1,
			sizeof(u64));

		d->d_ino = ns_pid->id;
		d->d_off = 0;
		d->d_reclen = reclen;
		d->d_type = DT_LNK;
		memset(d->d_name, 0, reclen - offsetof(struct linux_dirent64, d_name));
		memcpy(d->d_name, SHADOW_NS_NSFD_NAME_PID, n);
		extra_len += reclen;
	}
	if (ns_children) {
		struct linux_dirent64 *d =
			(struct linux_dirent64 *)(kbuf + extra_len);
		int n = strlen(SHADOW_NS_NSFD_NAME_PID_CHILD);
		unsigned short reclen = ALIGN(
			offsetof(struct linux_dirent64, d_name) + n + 1,
			sizeof(u64));

		d->d_ino = ns_children->id;
		d->d_off = 0;
		d->d_reclen = reclen;
		d->d_type = DT_LNK;
		memset(d->d_name, 0, reclen - offsetof(struct linux_dirent64, d_name));
		memcpy(d->d_name, SHADOW_NS_NSFD_NAME_PID_CHILD, n);
		extra_len += reclen;
	}

	if (extra_len && ret + extra_len <= count &&
	    !copy_to_user((char __user *)udirp + ret, kbuf, extra_len))
		ret += extra_len;

	shadow_ns_put(ns_pid);
	shadow_ns_put(ns_children);
	return ret;
}

static long shadow_ns_hook_getdents64(const struct pt_regs *regs)
{
	int fd = (int)shadow_ns_sys_arg0(regs);
	void __user *udirp =
		(void __user *)(uintptr_t)shadow_ns_sys_arg1(regs);
	unsigned int count = (unsigned int)shadow_ns_sys_arg2(regs);
	long ret = real_sys_getdents64(regs);
	struct shadow_ns *ns;
	char *kbuf, *out;
	long off, outlen;
	pid_t ns_dir_rpid;

	if (ret <= 0)
		return ret;

	if (shadow_ns_fd_is_proc_ns_dir(fd, &ns_dir_rpid))
		return shadow_ns_getdents64_append_ns_entries(udirp, count, ret,
							       ns_dir_rpid);

	if (!shadow_ns_fd_is_proc_root(fd))
		return ret;

	ns = shadow_ns_current_pidns();
	if (!ns)
		return ret;

	kbuf = kmalloc(ret, GFP_KERNEL);
	if (!kbuf)
		goto out_put;
	out = kmalloc(ret + 256, GFP_KERNEL);
	if (!out) {
		kfree(kbuf);
		goto out_put;
	}

	if (copy_from_user(kbuf, udirp, ret))
		goto out_free;

	outlen = 0;
	for (off = 0; off < ret; ) {
		struct linux_dirent64 *d =
			(struct linux_dirent64 *)(kbuf + off);
		size_t maxname, namelen;
		struct linux_dirent64 *nd;
		char pidbuf[12];
		long rpid;
		pid_t vpid;
		int n;
		unsigned short new_reclen;

		if (!d->d_reclen || off + d->d_reclen > ret)
			break;

		maxname = d->d_reclen - offsetof(struct linux_dirent64, d_name);
		namelen = strnlen(d->d_name, maxname);

		if (!namelen || namelen >= sizeof(pidbuf) ||
		    !shadow_ns_component_is_digits(d->d_name, namelen)) {
			memcpy(out + outlen, d, d->d_reclen);
			outlen += d->d_reclen;
			off += d->d_reclen;
			continue;
		}

		memcpy(pidbuf, d->d_name, namelen);
		pidbuf[namelen] = '\0';
		if (kstrtol(pidbuf, 10, &rpid) || rpid <= 0 ||
		    rpid > INT_MAX) {
			memcpy(out + outlen, d, d->d_reclen);
			outlen += d->d_reclen;
			off += d->d_reclen;
			continue;
		}

		vpid = shadow_ns_pidns_to_vpid(ns->pid, (pid_t)rpid);
		if (!vpid) {
			/* Not a member of this namespace: hide entirely. */
			off += d->d_reclen;
			continue;
		}

		n = snprintf(pidbuf, sizeof(pidbuf), "%d", vpid);
		new_reclen = ALIGN(offsetof(struct linux_dirent64, d_name) +
					n + 1, sizeof(u64));
		nd = (struct linux_dirent64 *)(out + outlen);
		nd->d_ino = d->d_ino;
		nd->d_off = d->d_off;
		nd->d_reclen = new_reclen;
		nd->d_type = d->d_type;
		memset(nd->d_name, 0,
		       new_reclen - offsetof(struct linux_dirent64, d_name));
		memcpy(nd->d_name, pidbuf, n);
		outlen += new_reclen;
		off += d->d_reclen;
	}

	if (outlen <= count && !copy_to_user(udirp, out, outlen))
		ret = outlen;
	/* else: leave `ret`/buffer untouched -- fail safe to the unfiltered
	 * real listing rather than risk corrupting the caller's buffer. */

out_free:
	kfree(kbuf);
	kfree(out);
out_put:
	shadow_ns_put(ns);
	return ret;
}

static const char * const shadow_ns_getdents64_names[] = {
	"__arm64_sys_getdents64", "__x64_sys_getdents64", "sys_getdents64",
	NULL,
};

static struct shadow_hook shadow_ns_getdents64_hook =
	SHADOW_HOOK(shadow_ns_getdents64_names, shadow_ns_hook_getdents64,
		    &real_sys_getdents64);

struct shadow_hook *shadow_ns_procfs_hooks[] = {
	&shadow_ns_openat2_hook,
	&shadow_ns_openat_hook,
	&shadow_ns_open_hook,
	&shadow_ns_readlinkat_hook,
	&shadow_ns_readlink_hook,
	&shadow_ns_getdents64_hook,
	NULL,
};
