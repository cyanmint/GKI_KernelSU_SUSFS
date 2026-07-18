// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_cgdevices - simulated cgroup device controller
 *
 * A standalone loadable kernel module that keeps an ioctl-configured shadow
 * device policy and, when explicitly bound to a real cgroup, enforces that
 * policy at real device-open entry points.
 *
 * Motivation
 * ----------
 * The cgroup v1 device controller (CONFIG_CGROUP_DEVICE) lets a container
 * runtime configure which devices a container may access by writing rules to
 * the devices.allow / devices.deny files in the cgroup hierarchy. When that
 * controller is absent there is no native rule store and no
 * devcgroup_check_permission() hook point to reuse.
 *
 * shadow_cgdevices keeps a parallel, ioctl-driven simulation. Userspace creates
 * virtual "shadow cgroups", populates them with allow/deny rules that mirror
 * the OCI spec, and may bind the calling task's *real* cgroup identity to one
 * of those virtual cgroups. Once bound, subsequent device opens from that real
 * cgroup are checked transparently in-kernel.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <linux/refcount.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/hashtable.h>
#if IS_ENABLED(CONFIG_CGROUPS)
#include <linux/cgroup.h>
#endif

#include "shadow_hook.h"
#include "include/uapi/shadow_cgdevices.h"

#define SHADOW_CGDEVICES_VERSION "2.0"

#define SHADOW_CGDEV_MAX_CGROUPS	65536
#define SHADOW_CGDEV_MAX_RULES_PER_CG	256
#define SHADOW_CGDEV_BIND_HASH_BITS	8

/*
 * struct cgdev_rule - one device allow/deny entry.
 * Rules are kept in insertion order; CHECK returns the last matching result.
 */
struct cgdev_rule {
	struct list_head node;
	u8		 dev_type; /* SHADOW_CGDEV_TYPE_* */
	u8		 access;   /* SHADOW_CGDEV_READ | WRITE | MKNOD */
	bool		 allow;
	s32		 major;    /* -1 = wildcard */
	s32		 minor;    /* -1 = wildcard */
};

/*
 * struct shadow_cgroup - a virtual cgroup.
 * @id:         stable id returned to userspace
 * @parent_id:  id of parent cgroup, 0 if none
 * @rules:      ordered list of cgdev_rule
 * @nrules:     current number of rules
 * @rules_lock: protects @rules and @nrules
 * @refcount:   held by the owning session, temporary lookups and real-cgroup
 *              bindings
 */
struct shadow_cgroup {
	u32			id;
	u32			parent_id;
	struct list_head	rules;
	u32			nrules;
	struct mutex		rules_lock;
	refcount_t		refcount;
};

/* Per-open-fd session. */
struct cgdev_session {
	struct list_head	owned;
	struct mutex		lock;
};

struct cgdev_owned_ref {
	struct list_head	 node;
	struct shadow_cgroup	 *cg;
};

struct cgdev_binding {
	struct hlist_node	node;
	u64			real_cgroup_id;
	struct shadow_cgroup	*cg;
};

/* Global registries: id -> shadow_cgroup and real-cgroup-id -> shadow_cgroup. */
static DEFINE_XARRAY_ALLOC1(cgdev_map);
static DEFINE_HASHTABLE(cgdev_bindings, SHADOW_CGDEV_BIND_HASH_BITS);
static DEFINE_MUTEX(cgdev_map_lock);
static atomic_t cgdev_count = ATOMIC_INIT(0);

static int (*real_chrdev_open)(struct inode *inode, struct file *filp);
static int (*real_blkdev_open)(struct inode *inode, struct file *filp);

static void cgdev_put(struct shadow_cgroup *cg);
static int shadow_chrdev_open(struct inode *inode, struct file *filp);
static int shadow_blkdev_open(struct inode *inode, struct file *filp);

static const char * const cgdev_chrdev_open_names[] = {
	"chrdev_open",
	NULL,
};

/*
 * Block-device open symbol naming has drifted more across kernels than the
 * character-device path. Only same-prototype candidates are safe here, so
 * block enforcement is best-effort and may legitimately be skipped.
 */
static const char * const cgdev_blkdev_open_names[] = {
	"blkdev_open",
	NULL,
};

static struct shadow_hook cgdev_chrdev_open_hook =
	SHADOW_HOOK(cgdev_chrdev_open_names, shadow_chrdev_open, &real_chrdev_open);
static struct shadow_hook cgdev_blkdev_open_hook =
	SHADOW_HOOK(cgdev_blkdev_open_names, shadow_blkdev_open, &real_blkdev_open);
static struct shadow_hook *cgdev_hooks[] = {
	&cgdev_chrdev_open_hook,
	&cgdev_blkdev_open_hook,
	NULL,
};

static bool cgdev_current_real_cgroup_id(u64 *real_cgroup_id)
{
#if IS_ENABLED(CONFIG_CGROUPS)
	struct cgroup *cgrp;

	/*
	 * task_dfl_cgroup() has been the long-stable helper across the Android GKI
	 * 5.10 -> 6.12 range we target here; if a downstream tree renames it, the
	 * out-of-tree build must carry the trivial compat shim.
	 */
	cgrp = task_dfl_cgroup(current);
	if (!cgrp)
		return false;

	*real_cgroup_id = cgroup_id(cgrp);
	return *real_cgroup_id != 0;
#else
	return false;
#endif
}

static struct cgdev_binding *cgdev_binding_lookup_locked(u64 real_cgroup_id)
{
	struct cgdev_binding *binding;

	hash_for_each_possible(cgdev_bindings, binding, node, real_cgroup_id) {
		if (binding->real_cgroup_id == real_cgroup_id)
			return binding;
	}

	return NULL;
}

static struct shadow_cgroup *cgdev_get(u32 id)
{
	struct shadow_cgroup *cg;

	if (!id)
		return NULL;
	mutex_lock(&cgdev_map_lock);
	cg = xa_load(&cgdev_map, id);
	if (cg && !refcount_inc_not_zero(&cg->refcount))
		cg = NULL;
	mutex_unlock(&cgdev_map_lock);
	return cg;
}

static struct shadow_cgroup *cgdev_get_bound(u64 real_cgroup_id)
{
	struct cgdev_binding *binding;
	struct shadow_cgroup *cg = NULL;

	mutex_lock(&cgdev_map_lock);
	binding = cgdev_binding_lookup_locked(real_cgroup_id);
	if (binding) {
		cg = binding->cg;
		if (!refcount_inc_not_zero(&cg->refcount))
			cg = NULL;
	}
	mutex_unlock(&cgdev_map_lock);

	return cg;
}

static void cgdev_free_rules(struct shadow_cgroup *cg)
{
	struct cgdev_rule *r, *tmp;

	list_for_each_entry_safe(r, tmp, &cg->rules, node) {
		list_del(&r->node);
		kfree(r);
	}
	cg->nrules = 0;
}

static void cgdev_unbind_all_for_shadow(struct shadow_cgroup *target)
{
	struct cgdev_binding *binding;
	unsigned int bucket;

	for (;;) {
		struct shadow_cgroup *bound_cg = NULL;

		mutex_lock(&cgdev_map_lock);
		hash_for_each(cgdev_bindings, bucket, binding, node) {
			if (binding->cg != target)
				continue;
			hash_del(&binding->node);
			bound_cg = binding->cg;
			kfree(binding);
			break;
		}
		mutex_unlock(&cgdev_map_lock);

		if (!bound_cg)
			break;
		cgdev_put(bound_cg);
	}
}

static void cgdev_put(struct shadow_cgroup *cg)
{
	if (!cg)
		return;
	if (refcount_dec_and_test(&cg->refcount)) {
		mutex_lock(&cgdev_map_lock);
		xa_erase(&cgdev_map, cg->id);
		mutex_unlock(&cgdev_map_lock);
		atomic_dec(&cgdev_count);
		cgdev_free_rules(cg);
		mutex_destroy(&cg->rules_lock);
		kfree(cg);
	}
}

static int cgdev_session_own(struct cgdev_session *s, struct shadow_cgroup *cg)
{
	struct cgdev_owned_ref *ref;

	ref = kzalloc(sizeof(*ref), GFP_KERNEL);
	if (!ref)
		return -ENOMEM;
	ref->cg = cg;
	list_add(&ref->node, &s->owned);
	return 0;
}

/* ---- ioctl handlers ---- */

static long cgdev_ioc_create(struct cgdev_session *s, void __user *arg)
{
	struct shadow_cgdev_create req;
	struct shadow_cgroup *cg;
	int ret;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	if (atomic_read(&cgdev_count) >= SHADOW_CGDEV_MAX_CGROUPS)
		return -ENOSPC;

	cg = kzalloc(sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return -ENOMEM;

	cg->parent_id = req.parent_id;
	INIT_LIST_HEAD(&cg->rules);
	mutex_init(&cg->rules_lock);
	refcount_set(&cg->refcount, 1);

	mutex_lock(&cgdev_map_lock);
	ret = xa_alloc(&cgdev_map, &cg->id, cg, XA_LIMIT(1, INT_MAX),
		       GFP_KERNEL);
	mutex_unlock(&cgdev_map_lock);
	if (ret) {
		mutex_destroy(&cg->rules_lock);
		kfree(cg);
		return ret;
	}
	atomic_inc(&cgdev_count);

	mutex_lock(&s->lock);
	ret = cgdev_session_own(s, cg);
	mutex_unlock(&s->lock);
	if (ret) {
		cgdev_put(cg);
		return ret;
	}

	req.id = cg->id;
	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long cgdev_ioc_destroy(struct cgdev_session *s, void __user *arg)
{
	struct cgdev_owned_ref *ref, *tmp;
	u32 id;
	bool found = false;

	if (copy_from_user(&id, arg, sizeof(id)))
		return -EFAULT;
	if (!id)
		return -EINVAL;

	mutex_lock(&s->lock);
	list_for_each_entry_safe(ref, tmp, &s->owned, node) {
		if (ref->cg->id == id) {
			list_del(&ref->node);
			cgdev_unbind_all_for_shadow(ref->cg);
			cgdev_put(ref->cg);
			kfree(ref);
			found = true;
			break;
		}
	}
	mutex_unlock(&s->lock);
	return found ? 0 : -ENOENT;
}

static long cgdev_ioc_rule_add(struct cgdev_session *s, void __user *arg)
{
	struct shadow_cgdev_rule req;
	struct shadow_cgroup *cg;
	struct cgdev_rule *rule;
	int ret = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.dev_type != SHADOW_CGDEV_TYPE_ALL &&
	    req.dev_type != SHADOW_CGDEV_TYPE_BLOCK &&
	    req.dev_type != SHADOW_CGDEV_TYPE_CHAR)
		return -EINVAL;
	if (req.access & ~(SHADOW_CGDEV_READ | SHADOW_CGDEV_WRITE |
			   SHADOW_CGDEV_MKNOD))
		return -EINVAL;

	cg = cgdev_get(req.cgroup_id);
	if (!cg)
		return -ENOENT;

	mutex_lock(&cg->rules_lock);
	if (cg->nrules >= SHADOW_CGDEV_MAX_RULES_PER_CG) {
		mutex_unlock(&cg->rules_lock);
		cgdev_put(cg);
		return -ENOSPC;
	}
	rule = kzalloc(sizeof(*rule), GFP_KERNEL);
	if (!rule) {
		mutex_unlock(&cg->rules_lock);
		cgdev_put(cg);
		return -ENOMEM;
	}
	rule->dev_type = req.dev_type;
	rule->access   = req.access;
	rule->allow    = !!req.allow;
	rule->major    = req.major;
	rule->minor    = req.minor;
	list_add_tail(&rule->node, &cg->rules);
	cg->nrules++;
	mutex_unlock(&cg->rules_lock);

	cgdev_put(cg);
	return ret;
}

static long cgdev_ioc_rule_reset(struct cgdev_session *s, void __user *arg)
{
	struct shadow_cgroup *cg;
	u32 id;

	if (copy_from_user(&id, arg, sizeof(id)))
		return -EFAULT;

	cg = cgdev_get(id);
	if (!cg)
		return -ENOENT;

	mutex_lock(&cg->rules_lock);
	cgdev_free_rules(cg);
	mutex_unlock(&cg->rules_lock);

	cgdev_put(cg);
	return 0;
}

static long cgdev_ioc_bind(struct cgdev_session *s, void __user *arg)
{
	struct shadow_cgdev_bind req;
	struct shadow_cgroup *new_cg = NULL;
	struct shadow_cgroup *old_cg = NULL;
	struct cgdev_binding *binding;
	u64 real_cgroup_id;
	int ret = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	if (req.flags)
		return -EINVAL;
	if (!cgdev_current_real_cgroup_id(&real_cgroup_id))
		return -EOPNOTSUPP;

	req.real_cgroup_id = real_cgroup_id;

	if (req.cgroup_id) {
		new_cg = cgdev_get(req.cgroup_id);
		if (!new_cg)
			return -ENOENT;
	}

	mutex_lock(&cgdev_map_lock);
	binding = cgdev_binding_lookup_locked(real_cgroup_id);
	if (!binding) {
		if (!new_cg)
			goto out_unlock;

		binding = kzalloc(sizeof(*binding), GFP_KERNEL);
		if (!binding) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		binding->real_cgroup_id = real_cgroup_id;
		binding->cg = new_cg;
		hash_add(cgdev_bindings, &binding->node, real_cgroup_id);
		new_cg = NULL;
		goto out_unlock;
	}

	if (!new_cg) {
		hash_del(&binding->node);
		old_cg = binding->cg;
		kfree(binding);
		goto out_unlock;
	}

	if (binding->cg == new_cg)
		goto out_unlock;

	old_cg = binding->cg;
	binding->cg = new_cg;
	new_cg = NULL;

out_unlock:
	mutex_unlock(&cgdev_map_lock);
	cgdev_put(old_cg);
	cgdev_put(new_cg);

	if (ret)
		return ret;
	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

/*
 * Check whether a device access is allowed by the cgroup's rule list.
 * Walk all rules; the *last* matching rule determines the result.
 * Default (no match) is deny.
 */
static bool cgdev_check_access(struct shadow_cgroup *cg, u8 dev_type,
				s32 major, s32 minor, u8 access)
{
	struct cgdev_rule *rule;
	bool allowed = false;

	mutex_lock(&cg->rules_lock);
	list_for_each_entry(rule, &cg->rules, node) {
		/* Match device type. */
		if (rule->dev_type != SHADOW_CGDEV_TYPE_ALL &&
		    rule->dev_type != dev_type)
			continue;
		/* Match major (wildcard = -1). */
		if (rule->major != -1 && rule->major != major)
			continue;
		/* Match minor (wildcard = -1). */
		if (rule->minor != -1 && rule->minor != minor)
			continue;
		/*
		 * Match access bits. access=0 means "any access" (wildcard), so a
		 * zero value always matches. For non-zero requests the rule must cover
		 * at least the requested bits.
		 */
		if (access && (rule->access & access) != access)
			continue;
		/* Last matching rule wins. */
		allowed = rule->allow;
	}
	mutex_unlock(&cg->rules_lock);
	return allowed;
}

static bool cgdev_check_current_task_access(struct inode *inode,
					   struct file *filp)
{
	struct shadow_cgroup *cg;
	u64 real_cgroup_id;
	u8 access = 0;
	u8 dev_type;
	bool allowed;

	if (!inode || !filp)
		return true;
	if (!cgdev_current_real_cgroup_id(&real_cgroup_id))
		return true;

	cg = cgdev_get_bound(real_cgroup_id);
	if (!cg)
		return true;

	if (filp->f_mode & FMODE_READ)
		access |= SHADOW_CGDEV_READ;
	if (filp->f_mode & FMODE_WRITE)
		access |= SHADOW_CGDEV_WRITE;

	dev_type = S_ISBLK(inode->i_mode) ? SHADOW_CGDEV_TYPE_BLOCK
					     : SHADOW_CGDEV_TYPE_CHAR;
	allowed = cgdev_check_access(cg, dev_type, imajor(inode), iminor(inode),
				     access);
	cgdev_put(cg);
	return allowed;
}

static int shadow_chrdev_open(struct inode *inode, struct file *filp)
{
	if (!cgdev_check_current_task_access(inode, filp))
		return -EPERM;
	return real_chrdev_open(inode, filp);
}

static int shadow_blkdev_open(struct inode *inode, struct file *filp)
{
	if (!cgdev_check_current_task_access(inode, filp))
		return -EPERM;
	return real_blkdev_open(inode, filp);
}

static long cgdev_ioc_check(struct cgdev_session *s, void __user *arg)
{
	struct shadow_cgdev_check req;
	struct shadow_cgroup *cg;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.dev_type != SHADOW_CGDEV_TYPE_BLOCK &&
	    req.dev_type != SHADOW_CGDEV_TYPE_CHAR)
		return -EINVAL;

	cg = cgdev_get(req.cgroup_id);
	if (!cg)
		return -ENOENT;

	req.allowed = cgdev_check_access(cg, req.dev_type, req.major,
					 req.minor, req.access) ? 1 : 0;
	cgdev_put(cg);

	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long cgdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cgdev_session *s = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case SHADOW_CGDEV_IOC_ABI_VERSION: {
		u32 ver = SHADOW_CGDEV_ABI_VERSION;

		return copy_to_user(uarg, &ver, sizeof(ver)) ? -EFAULT : 0;
	}
	case SHADOW_CGDEV_IOC_CREATE:	   return cgdev_ioc_create(s, uarg);
	case SHADOW_CGDEV_IOC_DESTROY:	   return cgdev_ioc_destroy(s, uarg);
	case SHADOW_CGDEV_IOC_RULE_ADD:	   return cgdev_ioc_rule_add(s, uarg);
	case SHADOW_CGDEV_IOC_RULE_RESET:  return cgdev_ioc_rule_reset(s, uarg);
	case SHADOW_CGDEV_IOC_BIND:	   return cgdev_ioc_bind(s, uarg);
	case SHADOW_CGDEV_IOC_CHECK:	   return cgdev_ioc_check(s, uarg);
	default:			   return -ENOTTY;
	}
}

static int cgdev_open(struct inode *inode, struct file *file)
{
	struct cgdev_session *s;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	INIT_LIST_HEAD(&s->owned);
	mutex_init(&s->lock);
	file->private_data = s;
	return 0;
}

static int cgdev_release(struct inode *inode, struct file *file)
{
	struct cgdev_session *s = file->private_data;
	struct cgdev_owned_ref *ref, *tmp;

	if (!s)
		return 0;

	list_for_each_entry_safe(ref, tmp, &s->owned, node) {
		list_del(&ref->node);
		cgdev_unbind_all_for_shadow(ref->cg);
		cgdev_put(ref->cg);
		kfree(ref);
	}
	mutex_destroy(&s->lock);
	kfree(s);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations cgdev_fops = {
	.owner		= THIS_MODULE,
	.open		= cgdev_open,
	.release	= cgdev_release,
	.unlocked_ioctl	= cgdev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice cgdev_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= SHADOW_CGDEV_DEVICE_NAME,
	.fops	= &cgdev_fops,
	.mode	= 0600,
};

int __init shadow_cgdevices_init(void)
{
	int hooked;
	int ret;

	pr_info("shadow_cgdevices: init: registering misc device %s\n",
		SHADOW_CGDEV_DEVICE_PATH);
	ret = misc_register(&cgdev_miscdev);
	if (ret) {
		pr_err("shadow_cgdevices: failed to register misc device: %d\n",
		       ret);
		return ret;
	}
	pr_info("shadow_cgdevices: init: misc device registered\n");

	pr_info("shadow_cgdevices: init: installing transparent syscall hooks\n");
	hooked = shadow_hook_install_all(cgdev_hooks, "shadow_cgdevices");
	if (hooked < 0) {
		pr_err("shadow_cgdevices: init: shadow_hook_install_all() failed: %d\n",
		       hooked);
		shadow_hook_remove_all(cgdev_hooks);
		misc_deregister(&cgdev_miscdev);
		return hooked;
	}
	pr_info("shadow_cgdevices: init: %d hook(s) installed\n", hooked);

	pr_info("shadow_cgdevices: loaded (ABI v%d) at %s; transparent hooks installed for %d symbol(s)\n",
		SHADOW_CGDEV_ABI_VERSION, SHADOW_CGDEV_DEVICE_PATH, hooked);
	return 0;
}

void shadow_cgdevices_exit(void)
{
	struct shadow_cgroup *cg;
	struct cgdev_binding *binding;
	struct hlist_node *tmp;
	unsigned long id;
	unsigned int bucket;

	pr_info("shadow_cgdevices: exit: removing transparent syscall hooks\n");
	shadow_hook_remove_all(cgdev_hooks);
	pr_info("shadow_cgdevices: exit: deregistering misc device\n");
	misc_deregister(&cgdev_miscdev);

	mutex_lock(&cgdev_map_lock);
	hash_for_each_safe(cgdev_bindings, bucket, tmp, binding, node) {
		hash_del(&binding->node);
		kfree(binding);
	}
	mutex_unlock(&cgdev_map_lock);

	mutex_lock(&cgdev_map_lock);
	xa_for_each(&cgdev_map, id, cg) {
		xa_erase(&cgdev_map, id);
		cgdev_free_rules(cg);
		mutex_destroy(&cg->rules_lock);
		kfree(cg);
	}
	mutex_unlock(&cgdev_map_lock);
	xa_destroy(&cgdev_map);

	pr_info("shadow_cgdevices: unloaded\n");
}

/*
 * Presence marker for shadow_ctr_checker (see shadow_sysvipc.c for rationale).
 */
int shadow_cgdevices_is_active(void)
{
	return 1;
}
EXPORT_SYMBOL_GPL(shadow_cgdevices_is_active);

module_init(shadow_cgdevices_init);
module_exit(shadow_cgdevices_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Simulated cgroup device-access controller (devices.allow/deny enforcement) with transparent syscall hijacking and /dev/shadow_cgdevices control device");
MODULE_VERSION(SHADOW_CGDEVICES_VERSION);
