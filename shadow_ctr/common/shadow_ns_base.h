/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shadow_ns_base - plugin/extension-point API exported by shadow_ns_base.ko
 *
 * shadow_ns_base.ko owns the generic "shadow namespace" registry, the
 * /dev/shadow_ns misc device + ioctl ABI, and the transparent unshare/setns/
 * clone/clone3/fork/vfork syscall hooks. Those operate uniformly across all
 * seven namespace types (see enum shadow_ns_type in the UAPI header) as
 * reference-counted bookkeeping, regardless of which — if any — per-type
 * submodule is loaded.
 *
 * A per-type submodule (shadow_ns_uts.ko, shadow_ns_net.ko, ...) may register
 * a small set of ops with shadow_ns_base to (a) declare that a namespace type
 * is intentionally enabled by the deployer, and (b) optionally attach a
 * private per-namespace payload plus type-specific ioctl handling. Today only
 * shadow_ns_uts.ko provides genuine functional behaviour (real nodename/
 * domainname storage, .real_support = true); the others are thin presence
 * modules (.real_support = false).
 *
 * struct shadow_ns and struct shadow_session are deliberately opaque here:
 * their layout is private to shadow_ns_base.c. Submodules only ever hold
 * pointers to them and reach the payload through the accessors below.
 */

#ifndef _SHADOW_NS_BASE_H
#define _SHADOW_NS_BASE_H

#include <linux/types.h>
#include <linux/module.h>
#include <linux/err.h>

struct shadow_ns;	/* opaque: a single shadow namespace object */
struct shadow_session;	/* opaque: per-open-fd /dev/shadow_ns state */

/*
 * struct shadow_ns_type_ops - per-namespace-type extension hooks.
 * @owner:        the registering module (normally THIS_MODULE).
 *                shadow_ns_base takes a per-call try_module_get() reference on
 *                it around every priv_alloc/priv_free/ioctl invocation, so the
 *                callback code cannot be unloaded mid-call. It is deliberately
 *                not pinned for the whole registration lifetime (that would
 *                make the submodule permanently un-unloadable).
 * @priv_alloc:   called right after a new shadow namespace of this type is
 *                allocated (before it is published). @parent_id/@parent_priv
 *                describe the namespace this one is derived from (both 0/NULL
 *                for a fresh/root instance). Return an opaque priv pointer
 *                (or NULL if the type keeps no payload) or ERR_PTR() on
 *                failure (which aborts the namespace creation). Optional.
 * @priv_free:    called right before the shadow namespace is freed, and also
 *                for every surviving namespace of this type when the type is
 *                unregistered (so the submodule reclaims all its payloads
 *                before its code unloads). Optional; required if @priv_alloc
 *                may return a non-NULL, non-error pointer.
 * @ioctl:        optional handler for ioctl(2) command numbers that
 *                shadow_ns_base's own /dev/shadow_ns dispatcher does not
 *                recognise. Called with the session lock held; @priv is the
 *                payload of the session's current namespace of this type (or
 *                NULL if the session has none). Return -ENOTTY for commands
 *                this type does not own, so shadow_ns_base can keep looking.
 * @real_support: true if this type provides genuine functional behaviour
 *                beyond shadow_ns_base's generic bookkeeping (only UTS today).
 */
struct shadow_ns_type_ops {
	struct module	*owner;
	void		*(*priv_alloc)(u32 parent_id, void *parent_priv);
	void		 (*priv_free)(void *priv);
	long		 (*ioctl)(struct shadow_session *s, unsigned int cmd,
				  unsigned long arg, void *priv);
	bool		 real_support;
};

/*
 * Register/unregister a per-type extension. @type is an enum shadow_ns_type
 * value. Registration fails with -EBUSY if the type already has ops, -EINVAL
 * for a bad type, or -ENODEV if shadow_ns_base is shutting down.
 */
int shadow_ns_base_register_type(u32 type, const struct shadow_ns_type_ops *ops);
void shadow_ns_base_unregister_type(u32 type);

/* Query registration/fidelity state of a type (used by shadow_ctr_checker). */
bool shadow_ns_base_type_loaded(u32 type);
bool shadow_ns_base_type_real(u32 type);

/*
 * shadow_ns_base_get_current - return the current task group's shadow
 * namespace of @type, with a reference held, or NULL if there is none.
 * Release it with shadow_ns_base_put().
 */
struct shadow_ns *shadow_ns_base_get_current(u32 type);
void shadow_ns_base_put(struct shadow_ns *ns);

/* Return the opaque per-type payload attached to @ns (or NULL). */
void *shadow_ns_base_priv(struct shadow_ns *ns);

#endif /* _SHADOW_NS_BASE_H */
