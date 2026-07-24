/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VENDOR_NS_INTERNAL_H
#define _VENDOR_NS_INTERNAL_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/mutex.h>
#include <linux/idr.h>
#include <linux/uidgid.h>
#include <linux/sched.h>
#include <linux/ns_common.h>
#include <linux/utsname.h>
#include <linux/user_namespace.h>
#include <linux/pid_namespace.h>
#include <linux/nsproxy.h>

#include "include/uapi/vendor_ns.h"

#define VENDOR_NS_VERSION	"1.0-vendored"
#define VENDOR_NS_TAG		"vendor_ns"

#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME	0x00000080
#endif

#define VENDOR_NS_ALL_FLAGS \
	(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWPID | \
	 CLONE_NEWNET | CLONE_NEWUSER | CLONE_NEWCGROUP | CLONE_NEWTIME)

#define VNS_RESERVED_PIDS	300
#define VNS_PID_MAX_DEFAULT	0x8000
#define VNS_TASK_HASH_BITS	10

struct shadow_hook;

struct vns_generic_namespace {
	struct ns_common ns;
};

struct vns_registry_entry {
	struct ns_common *ns;
	u32 type;
	struct list_head link;
};

struct vns_task {
	pid_t tgid;
	struct nsproxy *nsproxy;
	struct user_namespace *user_ns;
	struct hlist_node node;
};

struct vns_registry {
	struct mutex lock;
	struct list_head ns_list[VENDOR_NS_TYPE_MAX];
	unsigned long ns_count[VENDOR_NS_TYPE_MAX];
	unsigned long ns_created[VENDOR_NS_TYPE_MAX];
	unsigned int next_inum;
	struct uts_namespace *root_uts;
	struct user_namespace *root_user;
	struct pid_namespace *root_pid;
	struct vns_generic_namespace *root_generic[VENDOR_NS_TYPE_MAX];
	struct nsproxy *root_nsproxy;
	DECLARE_HASHTABLE(tasks, VNS_TASK_HASH_BITS);
	unsigned long task_count;
	unsigned long stat_unshare;
	unsigned long stat_setns;
	unsigned long stat_clone;
	unsigned long stat_uts_set;
	unsigned long stat_uts_get;
	unsigned long stat_pid_xlate;
	unsigned long stat_uid_xlate;
	unsigned long stat_proc_filtered;
};

extern struct vns_registry vendor_ns_registry;

int vns_ns_alloc_inum(struct ns_common *ns);
void vns_ns_free_inum(struct ns_common *ns);
void vns_ns_register(struct ns_common *ns, u32 type);
void vns_ns_unregister(struct ns_common *ns);
const char *vns_type_name(u32 type);

struct uts_namespace *vns_copy_utsname(unsigned long flags,
	struct user_namespace *user_ns, struct uts_namespace *old_ns);
void vns_free_uts_ns(struct uts_namespace *ns);
struct uts_namespace *vns_uts_root(void);

u32 vns_map_id_down(struct uid_gid_map *map, u32 id);
u32 vns_map_id_up(struct uid_gid_map *map, u32 id);
bool vns_in_userns(const struct user_namespace *ancestor,
	const struct user_namespace *child);
struct user_namespace *vns_user_root(void);
struct user_namespace *vns_create_user_ns_from_parent(struct user_namespace *parent);
struct user_namespace *vns_get_user_ns(struct user_namespace *ns);
void vns_put_user_ns(struct user_namespace *ns);
void vns_free_user_ns(struct user_namespace *ns);

struct pid_namespace *vns_pid_root(void);
struct pid_namespace *vns_create_pid_ns(struct pid_namespace *parent,
	struct user_namespace *user_ns);
struct pid_namespace *vns_copy_pid_ns(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *old_ns);
struct pid_namespace *vns_get_pid_ns(struct pid_namespace *ns);
void vns_put_pid_ns(struct pid_namespace *ns);
void vns_free_pid_ns(struct pid_namespace *ns);
void vns_disable_pid_allocation(struct pid_namespace *ns);
void vns_zap_pid_ns_processes(struct pid_namespace *ns);
int vns_get_pid_max(void);
int vns_alloc_pidnr(struct pid_namespace *ns);
void vns_free_pidnr(struct pid_namespace *ns, int nr);

struct pid *vns_alloc_pid(struct pid_namespace *ns, pid_t *set_tid,
	size_t set_tid_size);
void vns_free_pid(struct pid *pid);
struct pid *vns_find_pid_ns(int nr, struct pid_namespace *ns);
struct pid *vns_find_vpid(int nr);
struct task_struct *vns_pid_task(struct pid *pid, enum pid_type type);
struct pid *vns_find_get_pid(pid_t nr);
struct pid *vns_get_task_pid(struct task_struct *task, enum pid_type type);
pid_t vns_pid_nr_ns(struct pid *pid, struct pid_namespace *ns);
pid_t vns_pid_vnr(struct pid *pid);

struct vns_generic_namespace *vns_generic_root(u32 type);
struct vns_generic_namespace *vns_create_generic_ns(u32 type);
void vns_free_generic_ns(struct vns_generic_namespace *ns);

struct nsproxy *vns_nsproxy_root(void);
void vns_get_nsproxy(struct nsproxy *nsp);
void vns_put_nsproxy(struct nsproxy *nsp);
struct vns_task *vns_task_lookup(pid_t tgid);
struct nsproxy *vns_current_nsproxy(bool create);
struct user_namespace *vns_current_user_ns(bool create);
int vns_do_unshare(unsigned long flags);
int vns_do_setns_flags(unsigned long flags);
void vns_track_child(pid_t child_tgid);
void vns_track_child_flags(pid_t child_tgid, unsigned long flags);
int vns_uts_set(const char *name, size_t len, bool domain);
int vns_uts_get(char *out, size_t outlen, bool domain);
pid_t vns_pid_translate(pid_t real);
uid_t vns_uid_translate(uid_t id);
gid_t vns_gid_translate(gid_t id);
bool vns_pid_visible(pid_t real);
bool vns_in_child_pidns(void);
void vns_task_purge_all(void);

unsigned long vns_sys_arg0(const struct pt_regs *regs);
unsigned long vns_sys_arg1(const struct pt_regs *regs);
unsigned long vns_sys_arg2(const struct pt_regs *regs);
unsigned long vns_sys_arg3(const struct pt_regs *regs);

extern struct shadow_hook *vendor_ns_core_hooks[];
extern struct shadow_hook *vendor_ns_uts_hooks[];
extern struct shadow_hook *vendor_ns_pid_hooks[];
extern struct shadow_hook *vendor_ns_user_hooks[];
extern struct shadow_hook *vendor_ns_procfs_hooks[];

size_t vendor_ns_diag_snprintf(char *buf, size_t buflen);
size_t vendor_ns_diag_snprintf_type(u32 type, char *buf, size_t buflen);
int vendor_ns_init(void);
void vendor_ns_exit(void);

#endif
