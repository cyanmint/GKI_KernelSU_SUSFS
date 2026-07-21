// SPDX-License-Identifier: GPL-2.0
#include "shadow_mqueue_internal.h"
#include "lkm4ctr_log.h"

#define SHADOW_MQ_DEV_MQUEUE_PATH "/dev/mqueue"
#define SHADOW_MQ_DEV_MQUEUE_MODE 0755

/*
 * vfs_mkdir()'s signature has changed twice upstream: it gained a
 * struct user_namespace * first parameter in v5.12, which was replaced by a
 * struct mnt_idmap * in v6.3. Every target KMI's LINUX_VERSION_CODE matches
 * its nominal upstream base (5.10, 5.15, 6.1, 6.6, 6.12) closely enough that
 * these two thresholds correctly split them into the three known-verified
 * groups: 5.10 (no extra parameter), 5.15/6.1 (user_namespace), and
 * 6.6/6.12 (mnt_idmap).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
typedef int (*mq_vfs_mkdir_fn)(struct mnt_idmap *, struct inode *,
				struct dentry *, umode_t);
#define MQ_VFS_MKDIR(fn, dir, dentry, mode) \
	(fn)(&nop_mnt_idmap, (dir), (dentry), (mode))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
typedef int (*mq_vfs_mkdir_fn)(struct user_namespace *, struct inode *,
				struct dentry *, umode_t);
#define MQ_VFS_MKDIR(fn, dir, dentry, mode) \
	(fn)(&init_user_ns, (dir), (dentry), (mode))
#else
typedef int (*mq_vfs_mkdir_fn)(struct inode *, struct dentry *, umode_t);
#define MQ_VFS_MKDIR(fn, dir, dentry, mode) \
	(fn)((dir), (dentry), (mode))
#endif

typedef int (*mq_kern_path_fn)(const char *, unsigned int, struct path *);
typedef struct dentry *(*mq_kern_path_create_fn)(int, const char *,
						  struct path *, unsigned int);
typedef void (*mq_done_path_create_fn)(struct path *, struct dentry *);
typedef int (*mq_path_mount_fn)(const char *, struct path *, const char *,
				 unsigned long, void *);
typedef void (*mq_path_put_fn)(const struct path *);

/*
 * mq_dev_mqueue_do_mount() - mount tmpfs at an already-resolved @path.
 * @path is left untouched (caller still owns/puts the reference); returns
 * the underlying path_mount() result.
 *
 * The dev_name argument is "mqueue" (not "tmpfs") to mirror what
 * hook_sys_mount()'s reactive fallback passes through from the original
 * caller: it is only a cosmetic label recorded in /proc/mounts (tmpfs
 * itself ignores dev_name), and keeping it consistent with the reactive
 * path avoids a confusing mismatch between the two code paths' mounts.
 */
static int mq_dev_mqueue_do_mount(mq_path_mount_fn path_mount_fn,
				   struct path *path)
{
	return path_mount_fn("mqueue", path, "tmpfs",
			      MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
}

static void mq_dev_mqueue_ensure(void)
{
	mq_kern_path_fn kern_path_fn;
	mq_kern_path_create_fn kern_path_create_fn;
	mq_done_path_create_fn done_path_create_fn;
	mq_vfs_mkdir_fn vfs_mkdir_fn;
	mq_path_mount_fn path_mount_fn;
	mq_path_put_fn path_put_fn;
	struct path path;
	struct path create_path;
	struct dentry *dentry;
	int ret;

	kern_path_fn = (mq_kern_path_fn)shadow_hook_resolve("kern_path");
	kern_path_create_fn = (mq_kern_path_create_fn)
		shadow_hook_resolve("kern_path_create");
	done_path_create_fn = (mq_done_path_create_fn)
		shadow_hook_resolve("done_path_create");
	vfs_mkdir_fn = (mq_vfs_mkdir_fn)shadow_hook_resolve("vfs_mkdir");
	path_mount_fn = (mq_path_mount_fn)shadow_hook_resolve("path_mount");
	path_put_fn = (mq_path_put_fn)shadow_hook_resolve("path_put");

	if (!kern_path_fn || !kern_path_create_fn || !done_path_create_fn ||
	    !vfs_mkdir_fn || !path_mount_fn || !path_put_fn) {
		LKM4CTR_INFO("shadow_mqueue", "init: could not resolve VFS helpers for proactive %s mount; falling back to the reactive mount(2) hook only", SHADOW_MQ_DEV_MQUEUE_PATH);
		return;
	}

	ret = kern_path_fn(SHADOW_MQ_DEV_MQUEUE_PATH, LOOKUP_DIRECTORY, &path);
	if (!ret) {
		/*
		 * Something is already mounted at this path (real mqueue, a
		 * previous tmpfs fallback, ...): leave it alone. A dentry
		 * returned by a successful kern_path() always has a valid
		 * ->d_sb (every dentry belongs to a superblock for as long as
		 * it exists), so path.dentry->d_sb is safe to dereference
		 * here without a NULL check.
		 */
		if (path.dentry == path.dentry->d_sb->s_root) {
			LKM4CTR_INFO("shadow_mqueue", "init: %s is already a mountpoint; leaving it as-is", SHADOW_MQ_DEV_MQUEUE_PATH);
			path_put_fn(&path);
			return;
		}

		ret = mq_dev_mqueue_do_mount(path_mount_fn, &path);
		path_put_fn(&path);
		if (ret)
			LKM4CTR_INFO("shadow_mqueue", "init: proactive tmpfs mount on existing %s failed: %d", SHADOW_MQ_DEV_MQUEUE_PATH, ret);
		else
			LKM4CTR_INFO("shadow_mqueue", "init: mounted tmpfs on existing %s", SHADOW_MQ_DEV_MQUEUE_PATH);
		return;
	}

	if (ret != -ENOENT) {
		LKM4CTR_INFO("shadow_mqueue", "init: kern_path(%s) failed: %d", SHADOW_MQ_DEV_MQUEUE_PATH, ret);
		return;
	}

	/* /dev/mqueue does not exist yet: create it, then mount tmpfs on it. */
	dentry = kern_path_create_fn(AT_FDCWD, SHADOW_MQ_DEV_MQUEUE_PATH,
				      &create_path, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry)) {
		LKM4CTR_INFO("shadow_mqueue", "init: kern_path_create(%s) failed: %ld", SHADOW_MQ_DEV_MQUEUE_PATH, PTR_ERR(dentry));
		return;
	}

	ret = MQ_VFS_MKDIR(vfs_mkdir_fn, d_inode(create_path.dentry), dentry,
			    SHADOW_MQ_DEV_MQUEUE_MODE);
	done_path_create_fn(&create_path, dentry);
	if (ret) {
		LKM4CTR_INFO("shadow_mqueue", "init: mkdir(%s) failed: %d", SHADOW_MQ_DEV_MQUEUE_PATH, ret);
		return;
	}

	ret = kern_path_fn(SHADOW_MQ_DEV_MQUEUE_PATH, LOOKUP_DIRECTORY, &path);
	if (ret) {
		LKM4CTR_INFO("shadow_mqueue", "init: kern_path(%s) failed after mkdir: %d", SHADOW_MQ_DEV_MQUEUE_PATH, ret);
		return;
	}

	ret = mq_dev_mqueue_do_mount(path_mount_fn, &path);
	path_put_fn(&path);
	if (ret)
		LKM4CTR_INFO("shadow_mqueue", "init: created %s but tmpfs mount failed: %d", SHADOW_MQ_DEV_MQUEUE_PATH, ret);
	else
		LKM4CTR_INFO("shadow_mqueue", "init: created and mounted %s", SHADOW_MQ_DEV_MQUEUE_PATH);
}

int shadow_mqueue_init(void)
{
	int ret;

	LKM4CTR_INFO("shadow_mqueue", "init: installing transparent mq_*/mount hooks");
	ret = shadow_mqueue_install_hooks();
	if (ret < 0) {
		LKM4CTR_ERR("shadow_mqueue", "init: shadow_hook_install_all() failed: %d", ret);
		shadow_mqueue_remove_hooks();
		return ret;
	}
	LKM4CTR_INFO("shadow_mqueue", "init: %d hook(s) installed", ret);

	mq_dev_mqueue_ensure();

	LKM4CTR_INFO("shadow_mqueue", "simulated POSIX mqueue subsystem loaded with transparent mq_* hooks");
	return 0;
}

void shadow_mqueue_exit(void)
{
	LKM4CTR_INFO("shadow_mqueue", "exit: removing transparent mq_* hooks");
	shadow_mqueue_remove_hooks();
	mq_release_all();
	LKM4CTR_INFO("shadow_mqueue", "simulated POSIX mqueue subsystem unloaded");
}

/*
 * Presence marker for lkm4ctr_checker (see shadow_sysvipc.c for rationale).
 */
int shadow_mqueue_is_active(void)
{
	return 1;
}
EXPORT_SYMBOL_GPL(shadow_mqueue_is_active);
