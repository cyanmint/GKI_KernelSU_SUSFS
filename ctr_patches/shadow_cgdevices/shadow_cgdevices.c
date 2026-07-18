// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_cgdevices - simulated cgroup device controller
 *
 * A standalone loadable kernel module that provides bookkeeping for per-
 * container device-access policies through ioctls on /dev/shadow_cgdevices.
 *
 * Motivation
 * ----------
 * The cgroup v1 device controller (CONFIG_CGROUP_DEVICE) lets a container
 * runtime configure which devices a container may access by writing rules to
 * the devices.allow / devices.deny files in the cgroup hierarchy.  When the
 * controller is absent those writes fail and the runtime cannot enforce device
 * restrictions.
 *
 * shadow_cgdevices provides a parallel, ioctl-driven simulation.  A patched
 * containerd/runc creates virtual "shadow cgroups", populates them with allow
 * and deny rules that mirror the OCI spec, and queries the module to check
 * whether a given device access is permitted according to those rules.
 *
 * What is simulated
 * -----------------
 * - Virtual cgroup objects with parent/child identity and stable integer ids.
 * - An ordered allow/deny rule list per cgroup matching the cgroup v1 device
 *   controller format: (type, major, minor, access, allow|deny).
 * - A CHECK ioctl that walks the rule list and returns the last-matching
 *   decision (default: deny).
 * - Reference counting tied to the open fd; cgroups are freed when their
 *   creating session closes.
 *
 * What is NOT simulated
 * ---------------------
 * Actual device-access enforcement.  Deciding whether a /dev node can be
 * opened requires kernel support (LSM hooks, BPF, or the native cgroup device
 * controller).  This module tracks the *intended* policy so a patched runtime
 * can proceed without failing.
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

#include "include/uapi/shadow_cgdevices.h"

#define SHADOW_CGDEV_MAX_CGROUPS	65536
#define SHADOW_CGDEV_MAX_RULES_PER_CG	256

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
 * @refcount:   dropped by the owning session
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
	struct list_head	  node;
	struct shadow_cgroup	 *cg;
};

/* Global registry: id -> shadow_cgroup. */
static DEFINE_XARRAY_ALLOC1(cgdev_map);
static DEFINE_MUTEX(cgdev_map_lock);
static atomic_t cgdev_count = ATOMIC_INIT(0);

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

static void cgdev_free_rules(struct shadow_cgroup *cg)
{
	struct cgdev_rule *r, *tmp;

	list_for_each_entry_safe(r, tmp, &cg->rules, node) {
		list_del(&r->node);
		kfree(r);
	}
	cg->nrules = 0;
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
		 * Match access bits.  access=0 means "any access" (wildcard),
		 * so a zero value always matches.  For non-zero requests the
		 * rule must cover at least the requested bits.
		 */
		if (access && !(rule->access & access))
			continue;
		/* Last matching rule wins. */
		allowed = rule->allow;
	}
	mutex_unlock(&cg->rules_lock);
	return allowed;
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
	.llseek		= no_llseek,
};

static struct miscdevice cgdev_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= SHADOW_CGDEV_DEVICE_NAME,
	.fops	= &cgdev_fops,
	.mode	= 0600,
};

static int __init shadow_cgdevices_init(void)
{
	int ret;

	ret = misc_register(&cgdev_miscdev);
	if (ret) {
		pr_err("shadow_cgdevices: failed to register misc device: %d\n",
		       ret);
		return ret;
	}
	pr_info("shadow_cgdevices: simulated cgroup device controller loaded (ABI v%d) at %s\n",
		SHADOW_CGDEV_ABI_VERSION, SHADOW_CGDEV_DEVICE_PATH);
	return 0;
}

static void __exit shadow_cgdevices_exit(void)
{
	struct shadow_cgroup *cg;
	unsigned long id;

	misc_deregister(&cgdev_miscdev);

	mutex_lock(&cgdev_map_lock);
	xa_for_each(&cgdev_map, id, cg) {
		xa_erase(&cgdev_map, id);
		cgdev_free_rules(cg);
		mutex_destroy(&cg->rules_lock);
		kfree(cg);
	}
	mutex_unlock(&cgdev_map_lock);
	xa_destroy(&cgdev_map);

	pr_info("shadow_cgdevices: simulated cgroup device controller unloaded\n");
}

module_init(shadow_cgdevices_init);
module_exit(shadow_cgdevices_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("GKI_KernelSU_SUSFS contributors");
MODULE_DESCRIPTION("Simulated cgroup device controller for patched containerd");
MODULE_VERSION("1.0");
