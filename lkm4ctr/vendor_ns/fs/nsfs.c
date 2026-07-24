// SPDX-License-Identifier: GPL-2.0
/*
 * Vendored from kernel-common fs/nsfs.c (kernel version 6.1.124,
 * android14-6.1 branch). See lkm4ctr/vendor_ns/README.md for the vendoring
 * rules this file follows. Only the changes marked "RENAME", "DIAGFS
 * PLUMBING" or "STANDALONE COMPILE" below differ from the pristine kernel
 * source; everything else is intentionally kept close to the original.
 */

#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/magic.h>
#include <linux/ktime.h>
#include <linux/seq_file.h>
#include <linux/user_namespace.h>
#include <linux/nsfs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "../vendor_ns.h"

#ifndef PROC_DYNAMIC_FIRST
#define PROC_DYNAMIC_FIRST 0xF0000000U
#endif

/*
 * STANDALONE COMPILE: the real nsfs mount is created at boot and proc_alloc_inum()
 * / proc_free_inum() live in unrelated procfs code that is not exported to
 * modules. vendor_ns only needs the inode-number allocator and small bits of the
 * path helper surface, so we keep a private allocator and make path helpers fail
 * cleanly when no private mount exists.
 */
static struct vfsmount *vns_nsfs_mnt;
static DEFINE_SPINLOCK(vns_inum_lock);

/* DIAGFS PLUMBING: keep a side registry keyed by real struct ns_common*. */
static struct vns_registry_entry *vns_find_registry_entry(struct ns_common *ns)
{
	struct vns_registry_entry *entry;
	u32 type;

	for (type = 0; type < VENDOR_NS_TYPE_MAX; type++) {
		list_for_each_entry(entry, &vendor_ns_registry.ns_list[type], link) {
			if (entry->ns == ns)
				return entry;
		}
	}
	return NULL;
}

int vns_ns_alloc_inum(struct ns_common *ns) /* RENAME */
{
	unsigned long flags;

	spin_lock_irqsave(&vns_inum_lock, flags);
	if (vendor_ns_registry.next_inum < PROC_DYNAMIC_FIRST)
		vendor_ns_registry.next_inum = PROC_DYNAMIC_FIRST; /* STANDALONE COMPILE */
	ns->inum = vendor_ns_registry.next_inum++;
	spin_unlock_irqrestore(&vns_inum_lock, flags);
	return 0;
}

void vns_ns_free_inum(struct ns_common *ns) /* RENAME */
{
	/* STANDALONE COMPILE: private allocator is monotonic; nothing to recycle. */
	ns->inum = 0;
}

void vns_ns_register(struct ns_common *ns, u32 type) /* DIAGFS PLUMBING */
{
	struct vns_registry_entry *entry;

	if (!ns || type >= VENDOR_NS_TYPE_MAX)
		return;
	if (vns_find_registry_entry(ns))
		return;
	entry = kzalloc(sizeof(*entry), GFP_KERNEL); /* STANDALONE COMPILE */
	if (!entry)
		return;
	entry->ns = ns;
	entry->type = type;
	list_add_tail(&entry->link, &vendor_ns_registry.ns_list[type]);
	vendor_ns_registry.ns_count[type]++;
	vendor_ns_registry.ns_created[type]++;
}

void vns_ns_unregister(struct ns_common *ns) /* DIAGFS PLUMBING */
{
	struct vns_registry_entry *entry = vns_find_registry_entry(ns);

	if (!entry)
		return;
	list_del(&entry->link);
	if (vendor_ns_registry.ns_count[entry->type])
		vendor_ns_registry.ns_count[entry->type]--;
	kfree(entry);
}

const char *vns_type_name(u32 type)
{
	switch (type) {
	case VENDOR_NS_TYPE_UTS: return "uts";
	case VENDOR_NS_TYPE_IPC: return "ipc";
	case VENDOR_NS_TYPE_MNT: return "mnt";
	case VENDOR_NS_TYPE_PID: return "pid";
	case VENDOR_NS_TYPE_NET: return "net";
	case VENDOR_NS_TYPE_USER: return "user";
	case VENDOR_NS_TYPE_CGROUP: return "cgroup";
	case VENDOR_NS_TYPE_TIME: return "time";
	default: return "?";
	}
}

static long vns_ns_ioctl(struct file *filp, unsigned int ioctl,
			 unsigned long arg);
static const struct file_operations vns_ns_file_operations = {
	.llseek		= noop_llseek,
	.unlocked_ioctl = vns_ns_ioctl,
};

typedef struct ns_common *vns_ns_get_path_helper_t(void *private_data);

static int vns___ns_get_path(struct path *path, struct ns_common *ns) /* RENAME */
{
	struct vfsmount *mnt = vns_nsfs_mnt;
	struct dentry *dentry;
	struct inode *inode;

	if (!mnt) {
		if (ns->ops && ns->ops->put)
			ns->ops->put(ns);
		return -ENOENT;
	}
	inode = new_inode_pseudo(mnt->mnt_sb);
	if (!inode) {
		if (ns->ops && ns->ops->put)
			ns->ops->put(ns);
		return -ENOMEM;
	}
	inode->i_ino = ns->inum;
	inode->i_flags |= S_IMMUTABLE;
	inode->i_mode = S_IFREG | 0444;
	inode->i_fop = &vns_ns_file_operations;
	inode->i_private = ns;
	dentry = d_alloc_anon(mnt->mnt_sb);
	if (!dentry) {
		iput(inode);
		return -ENOMEM;
	}
	d_instantiate(dentry, inode);
	dentry->d_fsdata = (void *)ns->ops;
	path->mnt = mntget(mnt);
	path->dentry = dentry;
	return 0;
}

static int vns_ns_get_path_cb(struct path *path, vns_ns_get_path_helper_t *ns_get_cb,
		       void *private_data) /* RENAME */
{
	int ret;

	do {
		struct ns_common *ns = ns_get_cb(private_data);
		if (!ns)
			return -ENOENT;
		ret = vns___ns_get_path(path, ns); /* RENAME */
	} while (ret == -EAGAIN);
	return ret;
}

struct vns_ns_get_path_task_args {
	const struct proc_ns_operations *ns_ops;
	struct task_struct *task;
};

static struct ns_common *vns_ns_get_path_task(void *private_data)
{
	struct vns_ns_get_path_task_args *args = private_data;
	return args->ns_ops->get(args->task);
}

static int vns_ns_get_path(struct path *path, struct task_struct *task,
		    const struct proc_ns_operations *ns_ops) /* RENAME */
{
	struct vns_ns_get_path_task_args args = {
		.ns_ops = ns_ops,
		.task = task,
	};
	return vns_ns_get_path_cb(path, vns_ns_get_path_task, &args);
}

static int vns_open_related_ns(struct ns_common *ns,
		struct ns_common *(*get_ns)(struct ns_common *ns)) /* RENAME */
{
	struct path path = {};
	struct file *f;
	int err;
	int fd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	do {
		struct ns_common *relative = get_ns(ns);
		if (IS_ERR(relative)) {
			put_unused_fd(fd);
			return PTR_ERR(relative);
		}
		err = vns___ns_get_path(&path, relative); /* RENAME */
	} while (err == -EAGAIN);
	if (err) {
		put_unused_fd(fd);
		return err;
	}
	f = dentry_open(&path, O_RDONLY, current_cred());
	path_put(&path);
	if (IS_ERR(f)) {
		put_unused_fd(fd);
		fd = PTR_ERR(f);
	} else {
		fd_install(fd, f);
	}
	return fd;
}

static long vns_ns_ioctl(struct file *filp, unsigned int ioctl,
			 unsigned long arg)
{
	return -ENOTTY; /* STANDALONE COMPILE: vendor_ns does not expose real nsfs ioctls */
}

/*
 * STANDALONE COMPILE: the remaining pristine nsfs.c surface (ns_match(),
 * ns_get_name(), proc_ns_fget(), filesystem registration and init) is unused by
 * vendor_ns's shadow-hook plumbing and would require a private mounted nsfs
 * instance to be meaningful. It is intentionally omitted.
 */
