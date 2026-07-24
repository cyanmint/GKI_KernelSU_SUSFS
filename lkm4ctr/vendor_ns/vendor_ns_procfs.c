// SPDX-License-Identifier: GPL-2.0
/*
 * vendor_ns_procfs.c - /proc directory filtering for vendored pid namespaces.
 *
 * vendor_ns's own plumbing (not vendored kernel source). It intercepts
 * getdents64(2) and, when the target directory is the root of a procfs mount
 * and the calling thread group lives in a vendored (non-root) pid namespace,
 * removes the numeric /proc/<pid> entries whose pid is not visible in that
 * vendored namespace -- the same "filter the readdir stream" technique
 * shadow_ns uses, written from scratch against the linux_dirent64 layout.
 *
 * Fabricating /proc/<pid>/ns/<type> symlink targets and rewriting
 * /proc/<pid>/{stat,status} content the way a builtin pid namespace does is not
 * achievable purely from getdents64 filtering and is documented as a limitation
 * of the loadable-module approach in the README; the numeric-entry filtering
 * below is the portion that is both safe and self-contained.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/magic.h>

#include "vendor_ns.h"
#include "../../common/shadow_hook.h"
#include "../../common/lkm4ctr_compat.h"
#include "../../common/lkm4ctr_log.h"

/* Mirror of struct linux_dirent64 (fs/readdir.c / include/linux/dirent.h). */
struct vns_linux_dirent64 {
	u64		d_ino;
	s64		d_off;
	unsigned short	d_reclen;
	unsigned char	d_type;
	char		d_name[];
};

static long (*real_sys_getdents64)(const struct pt_regs *regs);

static const char * const vns_getdents64_names[] = {
	"__arm64_sys_getdents64", "__x64_sys_getdents64", "sys_getdents64",
	NULL,
};

/* True if @fd refers to the root directory of a procfs mount. */
static bool vns_fd_is_procfs(unsigned int fd)
{
	struct fd f = fdget(fd);
	bool is_proc = false;

	if (fd_empty(f))
		return false;

	if (fd_file(f)->f_inode &&
	    fd_file(f)->f_inode->i_sb &&
	    fd_file(f)->f_inode->i_sb->s_magic == PROC_SUPER_MAGIC)
		is_proc = true;

	fdput(f);
	return is_proc;
}

/* Numeric directory name -> pid, or -1 if not all digits. */
static int vns_name_to_pid(const char *name)
{
	int pid = 0;
	const char *p = name;

	if (!*p)
		return -1;
	for (; *p; p++) {
		if (*p < '0' || *p > '9')
			return -1;
		pid = pid * 10 + (*p - '0');
	}
	return pid;
}

static long vns_hook_getdents64(const struct pt_regs *regs)
{
	unsigned int fd = (unsigned int)vns_sys_arg0(regs);
	void __user *udirent = (void __user *)(uintptr_t)vns_sys_arg1(regs);
	long ret = real_sys_getdents64(regs);
	char *kbuf, *out;
	long in_pos = 0, out_pos = 0;

	if (ret <= 0 || !udirent)
		return ret;

	/* Fast path: nothing to hide unless we are in a vendored child pidns. */
	if (!vns_in_child_pidns())
		return ret;
	if (!vns_fd_is_procfs(fd))
		return ret;

	kbuf = kvmalloc(ret, GFP_KERNEL);
	if (!kbuf)
		return ret;
	out = kvmalloc(ret, GFP_KERNEL);
	if (!out) {
		kvfree(kbuf);
		return ret;
	}

	if (copy_from_user(kbuf, udirent, ret)) {
		kvfree(kbuf);
		kvfree(out);
		return ret;
	}

	while (in_pos < ret) {
		struct vns_linux_dirent64 *de =
			(struct vns_linux_dirent64 *)(kbuf + in_pos);
		unsigned short reclen = de->d_reclen;
		int pid;

		if (reclen < sizeof(*de) || in_pos + reclen > ret)
			break;

		pid = vns_name_to_pid(de->d_name);
		if (pid <= 0 || vns_pid_visible(pid)) {
			memcpy(out + out_pos, de, reclen);
			out_pos += reclen;
		} else {
			vendor_ns_registry.stat_proc_filtered++;
		}
		in_pos += reclen;
	}

	if (out_pos && out_pos <= ret) {
		if (copy_to_user(udirent, out, out_pos))
			out_pos = ret; /* on fault, leave original length */
		ret = out_pos;
	}

	kvfree(kbuf);
	kvfree(out);
	return ret;
}

static struct shadow_hook vns_getdents64_hook =
	SHADOW_HOOK(vns_getdents64_names, vns_hook_getdents64,
		    &real_sys_getdents64);

struct shadow_hook *vendor_ns_procfs_hooks[] = {
	&vns_getdents64_hook,
	NULL,
};
