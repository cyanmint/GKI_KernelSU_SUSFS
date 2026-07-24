// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendored from kernel-common kernel/utsname.c (kernel version 6.1.124,
 * android14-6.1 branch). See lkm4ctr/vendor_ns/README.md for the vendoring
 * rules this file follows. Only the changes marked "RENAME", "DIAGFS
 * PLUMBING" or "STANDALONE COMPILE" below differ from the pristine kernel
 * source; everything else (comments included) is unmodified.
 *
 *  Copyright (C) 2004 IBM Corporation
 *
 *  Author: Serge Hallyn <serue@us.ibm.com>
 */

#include <linux/export.h>
#define free_uts_ns vns_free_uts_ns /* STANDALONE COMPILE */
#include <linux/uts.h>
#include <linux/utsname.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>
#include <linux/sched/task.h>

#include "../vendor_ns.h"

/*
 * STANDALONE COMPILE: the real kernel's uts_sem (kernel/sys.c,
 * DECLARE_RWSEM(uts_sem)) is file-scope-global but not EXPORT_SYMBOL'd, so
 * it cannot be referenced from this out-of-tree module. vendor_ns's vendored
 * UTS namespaces are never touched by the real sethostname(2)/uname(2) path,
 * so a private rwsem protecting only the vendored copies is equivalent here.
 */
static DECLARE_RWSEM(vns_uts_sem);

/*
 * STANDALONE COMPILE: the real uts_ns_cache is set up by uts_ns_init(),
 * called from kernel start-of-day (fs_initcall). An out-of-tree module has
 * no equivalent hook to run that before the first clone_uts_ns(), so we
 * allocate with kzalloc()/kfree() directly below instead of a dedicated
 * kmem_cache, and drop uts_ns_cache/uts_ns_init() entirely.
 */

/*
 * STANDALONE COMPILE: inc_ucount()/dec_ucount() (kernel/ucount.c) are not
 * exported and are not part of the files vendored here (RLIMIT_NPROC-style
 * per-user accounting is unrelated to the namespace behaviour vendor_ns
 * hooks). Reduced to no-ops that always succeed; vendor_ns tracks its own
 * live-namespace counters via vns_ns_register()/vns_ns_unregister() instead
 * (see vendor_ns/glue/vendor_ns_diag.c).
 */
static struct ucounts *vns_inc_uts_namespaces(struct user_namespace *ns)
{
	return (struct ucounts *)1; /* STANDALONE COMPILE: dummy non-NULL sentinel */
}

static void vns_dec_uts_namespaces(struct ucounts *ucounts)
{
}

static struct uts_namespace *vns_create_uts_ns(void) /* RENAME: avoid colliding with the real create_uts_ns() */
{
	struct uts_namespace *uts_ns;

	uts_ns = kzalloc(sizeof(*uts_ns), GFP_KERNEL); /* STANDALONE COMPILE: no vendored slab cache, see above */
	if (uts_ns)
		refcount_set(&uts_ns->ns.count, 1);
	return uts_ns;
}

/*
 * Clone a new ns copying an original utsname, setting refcount to 1
 * @old_ns: namespace to clone
 * Return ERR_PTR(-ENOMEM) on error (failure to allocate), new ns otherwise
 */
static struct uts_namespace *vns_clone_uts_ns(struct user_namespace *user_ns, /* RENAME */
					  struct uts_namespace *old_ns)
{
	struct uts_namespace *ns;
	struct ucounts *ucounts;
	int err;

	err = -ENOSPC;
	ucounts = vns_inc_uts_namespaces(user_ns); /* RENAME: call our stub, see above */
	if (!ucounts)
		goto fail;

	err = -ENOMEM;
	ns = vns_create_uts_ns(); /* RENAME */
	if (!ns)
		goto fail_dec;

	err = vns_ns_alloc_inum(&ns->ns); /* RENAME: vendored fs/nsfs.c allocator (vendor_ns/fs/nsfs.c) */
	if (err)
		goto fail_free;

	ns->ucounts = ucounts;
	/* STANDALONE COMPILE: no ns.ops vtable vendored (see vns_free_uts_ns comment below); real kernel setns(2)/proc install path is unused here */

	down_read(&vns_uts_sem); /* RENAME: private lock, see above */
	memcpy(&ns->name, &old_ns->name, sizeof(ns->name));
	ns->user_ns = get_user_ns(user_ns);
	up_read(&vns_uts_sem);
	vns_ns_register(&ns->ns, VENDOR_NS_TYPE_UTS); /* DIAGFS PLUMBING: make this ns visible under ./mnt/vendor_ns/uts/namespaces */
	return ns;

fail_free:
	kfree(ns); /* STANDALONE COMPILE: kzalloc()'d above, no slab cache */
fail_dec:
	vns_dec_uts_namespaces(ucounts); /* RENAME */
fail:
	return ERR_PTR(err);
}

/*
 * Copy task tsk's utsname namespace, or clone it if flags
 * specifies CLONE_NEWUTS.  In latter case, changes to the
 * utsname of this process won't be seen by parent, and vice
 * versa.
 */
struct uts_namespace *vns_copy_utsname(unsigned long flags, /* RENAME */
	struct user_namespace *user_ns, struct uts_namespace *old_ns)
{
	struct uts_namespace *new_ns;

	BUG_ON(!old_ns);
	get_uts_ns(old_ns);

	if (!(flags & CLONE_NEWUTS))
		return old_ns;

	new_ns = vns_clone_uts_ns(user_ns, old_ns); /* RENAME */

	put_uts_ns(old_ns);
	return new_ns;
}

void vns_free_uts_ns(struct uts_namespace *ns) /* RENAME */
{
	vns_dec_uts_namespaces(ns->ucounts); /* RENAME */
	put_user_ns(ns->user_ns);
	vns_ns_unregister(&ns->ns); /* DIAGFS PLUMBING: drop from ./mnt/vendor_ns/uts/namespaces before freeing */
	vns_ns_free_inum(&ns->ns); /* RENAME: vendored fs/nsfs.c allocator */
	kfree(ns); /* STANDALONE COMPILE: kzalloc()'d above, no slab cache */
}

/*
 * STANDALONE COMPILE: utsns_get()/utsns_put()/utsns_install()/utsns_owner()
 * and the utsns_operations vtable (kernel-common kernel/utsname.c) implement
 * the real kernel's /proc/<pid>/ns/uts + setns(2) "install into task->nsproxy"
 * path. vendor_ns never touches the real task_struct->nsproxy (there is no
 * exported way to do so from a module), so this vtable has no caller here and
 * is dropped; vendor_ns_syscalls.c (glue) implements setns(2)/unshare(2)
 * against the vendored registry directly instead.
 */
