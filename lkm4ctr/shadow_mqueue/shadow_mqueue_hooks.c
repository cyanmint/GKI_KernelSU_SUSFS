// SPDX-License-Identifier: GPL-2.0
#include "shadow_mqueue_internal.h"
#include "lkm4ctr_log.h"

static long (*real_sys_mq_open)(const struct pt_regs *regs);
static long (*real_sys_mq_unlink)(const struct pt_regs *regs);
static long (*real_sys_mq_timedsend)(const struct pt_regs *regs);
static long (*real_sys_mq_timedreceive)(const struct pt_regs *regs);
static long (*real_sys_mq_notify)(const struct pt_regs *regs);
static long (*real_sys_mq_getsetattr)(const struct pt_regs *regs);
static long (*real_sys_mount)(const struct pt_regs *regs);

static long hook_sys_mq_open(const struct pt_regs *regs)
{
	const char __user *uname = (const char __user *)SHADOW_SYSCALL_ARG(regs, 0);
	int oflag = (int)SHADOW_SYSCALL_ARG(regs, 1);
	struct mq_attr __user *uattr =
		(struct mq_attr __user *)SHADOW_SYSCALL_ARG(regs, 3);
	struct shadow_mq_attr create_attr = {
		.mq_maxmsg = SHADOW_MQ_MAXMSG_DEF,
		.mq_msgsize = SHADOW_MQ_MSGSIZE_MAX,
	};
	struct mq_handle_entry *he;
	struct mq_attr attr;
	char name[SHADOW_MQ_NAME_MAX + 1];
	bool created = false;
	long ret;
	int fd;

	ret = real_sys_mq_open(regs);
	if (ret != -ENOSYS)
		return ret;

	ret = mq_copy_name_from_user(uname, name);
	if (ret)
		return ret;

	if ((oflag & O_CREAT) && uattr) {
		if (copy_from_user(&attr, uattr, sizeof(attr)))
			return -EFAULT;
		create_attr.mq_maxmsg = attr.mq_maxmsg;
		create_attr.mq_msgsize = attr.mq_msgsize;
	}

	ret = mq_do_open(name, mq_posix_to_shadow_oflag(oflag), &create_attr, &he,
			 NULL, &created);
	if (ret)
		return ret;

	fd = mq_create_anon_fd(name, he, oflag, created);
	return fd;
}

static long hook_sys_mq_unlink(const struct pt_regs *regs)
{
	const char __user *uname = (const char __user *)SHADOW_SYSCALL_ARG(regs, 0);
	char name[SHADOW_MQ_NAME_MAX + 1];
	long ret;

	ret = real_sys_mq_unlink(regs);
	if (ret != -ENOSYS)
		return ret;

	ret = mq_copy_name_from_user(uname, name);
	if (ret)
		return ret;

	return mq_do_unlink(name);
}

static long hook_sys_mq_timedsend(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)SHADOW_SYSCALL_ARG(regs, 0);
	const char __user *umsg = (const char __user *)SHADOW_SYSCALL_ARG(regs, 1);
	size_t msg_len = (size_t)SHADOW_SYSCALL_ARG(regs, 2);
	unsigned int prio = (unsigned int)SHADOW_SYSCALL_ARG(regs, 3);
	const struct __kernel_timespec __user *uabs =
		(const struct __kernel_timespec __user *)SHADOW_SYSCALL_ARG(regs, 4);
	struct mq_handle_entry *he;
	struct mq_wait_spec wait;
	u8 msg_data[SHADOW_MQ_MSGSIZE_MAX];
	long ret;

	he = mq_get_shadow_handle_from_fd(mqdes);
	if (IS_ERR(he))
		return PTR_ERR(he);
	if (!he)
		return real_sys_mq_timedsend(regs);

	if (msg_len > sizeof(msg_data)) {
		mq_handle_put(he);
		return -EMSGSIZE;
	}
	if (copy_from_user(msg_data, umsg, msg_len)) {
		mq_handle_put(he);
		return -EFAULT;
	}

	ret = mq_wait_spec_from_abs_timeout(&wait, READ_ONCE(he->oflag), uabs);
	if (!ret)
		ret = mq_do_send(he, msg_data, (u32)msg_len, prio, &wait);
	mq_handle_put(he);
	return ret;
}

static long hook_sys_mq_timedreceive(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)SHADOW_SYSCALL_ARG(regs, 0);
	char __user *umsg = (char __user *)SHADOW_SYSCALL_ARG(regs, 1);
	size_t msg_len = (size_t)SHADOW_SYSCALL_ARG(regs, 2);
	unsigned int __user *uprio =
		(unsigned int __user *)SHADOW_SYSCALL_ARG(regs, 3);
	const struct __kernel_timespec __user *uabs =
		(const struct __kernel_timespec __user *)SHADOW_SYSCALL_ARG(regs, 4);
	struct mq_handle_entry *he;
	struct mq_wait_spec wait;
	u8 msg_data[SHADOW_MQ_MSGSIZE_MAX];
	u32 len;
	u32 prio;
	long ret;

	he = mq_get_shadow_handle_from_fd(mqdes);
	if (IS_ERR(he))
		return PTR_ERR(he);
	if (!he)
		return real_sys_mq_timedreceive(regs);

	if (msg_len > sizeof(msg_data)) {
		mq_handle_put(he);
		return -EMSGSIZE;
	}

	ret = mq_wait_spec_from_abs_timeout(&wait, READ_ONCE(he->oflag), uabs);
	if (ret)
		goto out_put;

	len = (u32)msg_len;
	ret = mq_do_receive(he, msg_data, &len, &prio, &wait);
	if (ret)
		goto out_put;

	if (copy_to_user(umsg, msg_data, len)) {
		ret = -EFAULT;
		goto out_put;
	}
	if (uprio && put_user(prio, uprio)) {
		ret = -EFAULT;
		goto out_put;
	}

	ret = len;
out_put:
	mq_handle_put(he);
	return ret;
}

static long hook_sys_mq_notify(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)SHADOW_SYSCALL_ARG(regs, 0);
	struct mq_handle_entry *he;

	he = mq_get_shadow_handle_from_fd(mqdes);
	if (IS_ERR(he))
		return PTR_ERR(he);
	if (!he)
		return real_sys_mq_notify(regs);

	mq_handle_put(he);
	return -ENOSYS;
}

static long hook_sys_mq_getsetattr(const struct pt_regs *regs)
{
	mqd_t mqdes = (mqd_t)SHADOW_SYSCALL_ARG(regs, 0);
	const struct mq_attr __user *unew =
		(const struct mq_attr __user *)SHADOW_SYSCALL_ARG(regs, 1);
	struct mq_attr __user *uold =
		(struct mq_attr __user *)SHADOW_SYSCALL_ARG(regs, 2);
	struct mq_handle_entry *he;
	struct shadow_mq_attr newattr;
	struct shadow_mq_attr oldattr;
	struct mq_attr old;
	long ret;

	he = mq_get_shadow_handle_from_fd(mqdes);
	if (IS_ERR(he))
		return PTR_ERR(he);
	if (!he)
		return real_sys_mq_getsetattr(regs);

	if (unew) {
		if (copy_from_user(&old, unew, sizeof(old))) {
			ret = -EFAULT;
			goto out_put;
		}
		memset(&newattr, 0, sizeof(newattr));
		newattr.mq_flags = (old.mq_flags & O_NONBLOCK) ? SHADOW_MQ_O_NONBLOCK : 0;
		ret = mq_do_setattr(he, &newattr, uold ? &oldattr : NULL);
	} else {
		ret = mq_do_getattr(he, &oldattr);
	}
	if (ret)
		goto out_put;

	if (uold) {
		mq_fill_posix_attr_from_shadow(&oldattr, &old);
		if (copy_to_user(uold, &old, sizeof(old))) {
			ret = -EFAULT;
			goto out_put;
		}
	}

	ret = 0;
out_put:
	mq_handle_put(he);
	return ret;
}

#define SHADOW_MQ_MOUNT_FSTYPE_MAX 32
#define SHADOW_MQ_MOUNT_TYPE_ARG   2

/*
 * mq_do_mount_fallback() - retry a failed mount("mqueue", ...) as tmpfs.
 *
 * We cannot just call an in-kernel mount helper directly (do_mount()/
 * path_mount() are not exported, and the get_tree_nodev()/simple_fill_super()
 * pair used by an earlier revision to register a real "mqueue" pseudo-fs are
 * trimmed from production GKI kernels' exported-symbol table - see the
 * top-of-file comment). Instead we reuse the *real* mount(2) syscall
 * unmodified, just with its filesystem-type argument swapped out: vm_mmap()
 * (like mmap(2) itself) maps a throwaway anonymous page into the *calling
 * process's* address space - i.e. it returns an ordinary userspace address,
 * not a kernel one, so copy_to_user() below is the correct way to populate
 * it. We write "tmpfs" into that page, point a copy of the original pt_regs
 * at it instead of the caller's "mqueue" string, and call through to
 * @real_sys_mount with the copy. vm_mmap()/vm_munmap() are ordinary
 * EXPORT_SYMBOL() helpers used throughout the VFS/ELF loader, but -- like
 * path_put()/vfs_mkdir()/shmem_kernel_file_setup() elsewhere in this
 * repository's lkm4ctr modules -- some "certified"/production Android GKI
 * boot images build with CONFIG_TRIM_UNUSED_KSYMS, which strips their
 * ksymtab entries whenever no *other* module the vendor ships happens to
 * reference them, causing a hard "Unknown symbol" failure at insmod time.
 * Resolve them via shadow_hook_resolve() (kallsyms-based, unaffected by
 * trimming) instead of calling them directly. The mapping is a full page
 * because do_mmap() internally requires (and rounds up to) a page-aligned
 * length regardless of what is requested; only the first few bytes are ever
 * written or read.
 *
 * The original dev_name/dir_name/flags/data arguments are passed through
 * unchanged: tmpfs accepts the same handful of options ("mode=", "size=",
 * "uid=", "gid=", ...) that runtimes typically pass for /dev/mqueue.
 */
typedef unsigned long (*mq_vm_mmap_fn)(struct file *file, unsigned long addr,
					unsigned long len, unsigned long prot,
					unsigned long flag, unsigned long offset);
typedef int (*mq_vm_munmap_fn)(unsigned long start, size_t len);

static long mq_do_mount_fallback(const struct pt_regs *regs)
{
	struct pt_regs kregs;
	unsigned long scratch;
	long ret;
	static mq_vm_mmap_fn vm_mmap_fn;
	static mq_vm_munmap_fn vm_munmap_fn;

	if (!vm_mmap_fn)
		vm_mmap_fn = (mq_vm_mmap_fn)shadow_hook_resolve("vm_mmap");
	if (!vm_munmap_fn)
		vm_munmap_fn = (mq_vm_munmap_fn)shadow_hook_resolve("vm_munmap");
	if (!vm_mmap_fn || !vm_munmap_fn)
		return -ENOSYS;

	memcpy(&kregs, regs, sizeof(kregs));

	scratch = vm_mmap_fn(NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, 0);
	if (IS_ERR_VALUE(scratch))
		return (long)scratch;

	/* sizeof("tmpfs") is 6 and deliberately includes the trailing NUL, so
	 * the string real_sys_mount() reads back out of userspace below is
	 * itself NUL-terminated. */
	if (copy_to_user((void __user *)scratch, "tmpfs", sizeof("tmpfs"))) {
		vm_munmap_fn(scratch, PAGE_SIZE);
		return -EFAULT;
	}

	SHADOW_SYSCALL_SET_ARG(&kregs, SHADOW_MQ_MOUNT_TYPE_ARG, scratch);
	ret = real_sys_mount(&kregs);
	vm_munmap_fn(scratch, PAGE_SIZE);
	return ret;
}

/*
 * hook_sys_mount() - let the real mount(2) run first; only retry as tmpfs
 * when it failed with -ENODEV for fstype "mqueue" specifically. Every other
 * fstype, and every other error (permissions, busy target, ...), passes
 * through untouched.
 */
static long hook_sys_mount(const struct pt_regs *regs)
{
	const char __user *utype =
		(const char __user *)SHADOW_SYSCALL_ARG(regs, SHADOW_MQ_MOUNT_TYPE_ARG);
	char type[SHADOW_MQ_MOUNT_FSTYPE_MAX];
	long copied;
	long ret;

	ret = real_sys_mount(regs);
	if (ret != -ENODEV || !utype)
		return ret;

	copied = strncpy_from_user(type, utype, sizeof(type));
	if (copied < 0 || copied >= sizeof(type))
		return ret;
	/*
	 * strncpy_from_user()'s @count includes room for the trailing NUL: on
	 * success (string shorter than @count) it returns the string length
	 * and has already written the NUL at type[copied]; if the source
	 * string didn't fit, it returns exactly sizeof(type) with no NUL
	 * written at all (see include/linux/uaccess.h / lib/strncpy_from_user.c
	 * docs: "If @count is smaller than the length of the string, copies
	 * @count bytes and returns @count"), which the ">= sizeof(type)" check
	 * above already rejects. The explicit NUL-termination here is
	 * therefore belt-and-braces, not a correctness fix, but keeps
	 * strcmp() provably safe regardless of kernel version quirks.
	 */
	type[copied] = '\0';
	if (strcmp(type, "mqueue"))
		return ret;

	LKM4CTR_LOG("shadow_mqueue",
		    "mount(\"mqueue\", ...) failed with -ENODEV; retrying as tmpfs so the caller sees a working mountpoint");
	return mq_do_mount_fallback(regs);
}

static const char * const mq_open_hook_names[] = {
	"__arm64_sys_mq_open",
	"sys_mq_open",
	NULL,
};

static const char * const mq_unlink_hook_names[] = {
	"__arm64_sys_mq_unlink",
	"sys_mq_unlink",
	NULL,
};

static const char * const mq_timedsend_hook_names[] = {
	"__arm64_sys_mq_timedsend",
	"sys_mq_timedsend",
	NULL,
};

static const char * const mq_timedreceive_hook_names[] = {
	"__arm64_sys_mq_timedreceive",
	"sys_mq_timedreceive",
	NULL,
};

static const char * const mq_notify_hook_names[] = {
	"__arm64_sys_mq_notify",
	"sys_mq_notify",
	NULL,
};

static const char * const mq_getsetattr_hook_names[] = {
	"__arm64_sys_mq_getsetattr",
	"sys_mq_getsetattr",
	NULL,
};

static const char * const mount_hook_names[] = {
	"__arm64_sys_mount",
	"sys_mount",
	NULL,
};

static struct shadow_hook mq_open_hook =
	SHADOW_HOOK(mq_open_hook_names, hook_sys_mq_open, &real_sys_mq_open);
static struct shadow_hook mq_unlink_hook =
	SHADOW_HOOK(mq_unlink_hook_names, hook_sys_mq_unlink, &real_sys_mq_unlink);
static struct shadow_hook mq_timedsend_hook =
	SHADOW_HOOK(mq_timedsend_hook_names, hook_sys_mq_timedsend,
		    &real_sys_mq_timedsend);
static struct shadow_hook mq_timedreceive_hook =
	SHADOW_HOOK(mq_timedreceive_hook_names, hook_sys_mq_timedreceive,
		    &real_sys_mq_timedreceive);
static struct shadow_hook mq_notify_hook =
	SHADOW_HOOK(mq_notify_hook_names, hook_sys_mq_notify, &real_sys_mq_notify);
static struct shadow_hook mq_getsetattr_hook =
	SHADOW_HOOK(mq_getsetattr_hook_names, hook_sys_mq_getsetattr,
		    &real_sys_mq_getsetattr);
static struct shadow_hook mount_hook =
	SHADOW_HOOK(mount_hook_names, hook_sys_mount, &real_sys_mount);

static struct shadow_hook *shadow_mqueue_hooks[] = {
	&mq_open_hook,
	&mq_unlink_hook,
	&mq_timedsend_hook,
	&mq_timedreceive_hook,
	&mq_notify_hook,
	&mq_getsetattr_hook,
	&mount_hook,
	NULL,
};

int shadow_mqueue_install_hooks(void)
{
	return shadow_hook_install_all(shadow_mqueue_hooks, "shadow_mqueue");
}

void shadow_mqueue_remove_hooks(void)
{
	shadow_hook_remove_all(shadow_mqueue_hooks);
}
