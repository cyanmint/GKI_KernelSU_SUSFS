// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_sysvipc shared-memory attach/detach (shmat/shmdt).
 *
 * shadow_sysvipc_registry.c only tracks resource identity/bookkeeping (size,
 * key, permissions) for TYPE_SHM resources; this file adds the actual
 * memory-sharing data path so shmat(2)/shmdt(2) genuinely map shared pages
 * into the caller instead of merely falling through to -ENOSYS.
 *
 * Design (mirrors real ipc/shm.c's newseg()/do_shmat() closely enough for
 * real-world callers, simplified where a module cannot safely go further):
 *   - The segment's backing store is a single shmem ("tmpfs") file, created
 *     lazily on the *first* shmat(2) of a given shmid (shmem_kernel_file_
 *     setup() -- the same primitive real ipc/shm.c's newseg() uses to back
 *     a non-hugetlb SysV segment) and shared by every subsequent attach of
 *     that same id: every attaching process mmap()s the *same* file, so
 *     writes from one process are visible to every other attached process
 *     through the ordinary page cache, exactly like real SysV shared
 *     memory.
 *   - Each successful shmat(2) takes its own reference on the
 *     svipc_resource (so the segment and its backing file stay alive for as
 *     long as anything is still attached, even if the creating task group
 *     has already IPC_RMID'd or exited -- mirroring real SysV "marked for
 *     destruction, but destruction deferred until the last attach detaches"
 *     semantics) and records (mm, address, length, resource) in a small
 *     global attachment list so a later shmdt(2) -- which only receives an
 *     address, not a shmid -- can find and unmap exactly that mapping and
 *     drop the matching resource reference.
 *
 * Deliberately NOT vendored: SHM_RDONLY/SHM_REMAP/SHM_EXEC address-hint
 * placement quirks, hugetlb-backed segments, and SHM_LOCK/mlock accounting
 * -- out of scope for the common container use case (a plain read/write
 * shared mapping at an address the kernel itself chooses), the same class
 * of deliberate simplification as shadow_ns_pid.c's single-level namespace
 * nesting.
 */
#include "shadow_sysvipc_internal.h"
#include <linux/shmem_fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>

struct svipc_shm_attach {
	struct list_head	node;
	pid_t			tgid;
	unsigned long		addr;
	unsigned long		len;
	struct svipc_resource	*res;
};

static DEFINE_MUTEX(svipc_shm_attach_lock);
static LIST_HEAD(svipc_shm_attach_list);

static bool svipc_shm_tgid_alive(pid_t tgid)
{
	struct pid *pid;
	struct task_struct *task;
	bool alive = false;

	pid = find_get_pid(tgid);
	if (!pid)
		return false;
	task = get_pid_task(pid, PIDTYPE_TGID);
	if (task) {
		alive = true;
		put_task_struct(task);
	}
	put_pid(pid);
	return alive;
}

/*
 * svipc_shm_reap_dead() - opportunistically drop attachment bookkeeping
 * (and the resource reference each attach holds) for any task group that
 * has since exited without calling shmdt(2) first.
 *
 * There is no tracepoint/hook on process exit here (the same "lazy reaping
 * on the next intercepted syscall" trade-off shadow_sysvipc_tgid.c documents
 * for ownership records), so this is called from both svipc_sys_shmat() and
 * svipc_sys_shmdt() below. It deliberately does NOT call vm_munmap() for a
 * dead tgid's mapping: that task's entire address space (and therefore this
 * mapping) was already torn down by the kernel's own exit_mmap() when the
 * process exited: nothing is left to unmap, only our own bookkeeping
 * reference needs to be dropped.
 */
static void svipc_shm_reap_dead(void)
{
	struct svipc_shm_attach *att, *tmp;
	LIST_HEAD(dead);

	mutex_lock(&svipc_shm_attach_lock);
	list_for_each_entry_safe(att, tmp, &svipc_shm_attach_list, node) {
		if (!svipc_shm_tgid_alive(att->tgid))
			list_move(&att->node, &dead);
	}
	mutex_unlock(&svipc_shm_attach_lock);

	list_for_each_entry_safe(att, tmp, &dead, node) {
		list_del(&att->node);
		svipc_put(att->res);
		kfree(att);
	}
}


typedef struct file *(*shadow_shmem_kernel_file_setup_fn)(const char *name,
							   loff_t size,
							   unsigned long flags);
typedef unsigned long (*shadow_vm_mmap_fn)(struct file *file,
					   unsigned long addr,
					   unsigned long len,
					   unsigned long prot,
					   unsigned long flag,
					   unsigned long offset);
typedef int (*shadow_vm_munmap_fn)(unsigned long start, size_t len);

/*
 * shmem_kernel_file_setup()/vm_mmap()/vm_munmap() are all real,
 * EXPORT_SYMBOL()'d kernel functions, but (like path_put/kern_path
 * elsewhere in this repository's lkm4ctr modules) may be trimmed by
 * CONFIG_TRIM_UNUSED_KSYMS on a production GKI kernel if nothing else in
 * vmlinux happens to reference them; resolve them the same defensive way
 * via shadow_hook_resolve() (kallsyms-based, unaffected by trimming)
 * instead of calling them directly.
 */
static struct file *svipc_shm_kernel_file_setup(const char *name, loff_t size,
						unsigned long flags)
{
	static shadow_shmem_kernel_file_setup_fn fn;

	if (!fn)
		fn = (shadow_shmem_kernel_file_setup_fn)
			shadow_hook_resolve("shmem_kernel_file_setup");
	if (!fn)
		return ERR_PTR(-ENOSYS);
	return fn(name, size, flags);
}

static unsigned long svipc_shm_vm_mmap(struct file *file, unsigned long addr,
				       unsigned long len, unsigned long prot,
				       unsigned long flag, unsigned long offset)
{
	static shadow_vm_mmap_fn fn;

	if (!fn)
		fn = (shadow_vm_mmap_fn)shadow_hook_resolve("vm_mmap");
	if (!fn)
		return -ENOSYS;
	return fn(file, addr, len, prot, flag, offset);
}

static int svipc_shm_vm_munmap(unsigned long start, size_t len)
{
	static shadow_vm_munmap_fn fn;

	if (!fn)
		fn = (shadow_vm_munmap_fn)shadow_hook_resolve("vm_munmap");
	if (!fn)
		return -ENOSYS;
	return fn(start, len);
}

/* Lazily create @res's backing shmem file on the first shmat(2). Caller
 * must hold no lock; takes/drops res->shm_lock itself.
 */
static int svipc_shm_ensure_file(struct svipc_resource *res)
{
	struct file *file;
	int ret = 0;

	mutex_lock(&res->shm_lock);
	if (res->shm_file)
		goto out;

	file = svipc_shm_kernel_file_setup("SYSV_shadow", res->size, 0);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto out;
	}
	res->shm_file = file;
out:
	mutex_unlock(&res->shm_lock);
	return ret;
}

void svipc_shm_purge_locked(struct svipc_resource *res)
{
	if (res->shm_file) {
		fput(res->shm_file);
		res->shm_file = NULL;
	}
}

long svipc_sys_shmat(int shmid, const void __user *ushmaddr, int shmflg,
		    unsigned long *raddr)
{
	struct svipc_resource *res;
	unsigned long prot = PROT_READ;
	unsigned long addr;
	struct svipc_shm_attach *att;
	long ret;

	svipc_shm_reap_dead();

	res = svipc_get(shmid);
	if (!res)
		return -EINVAL;
	if (res->type != SHADOW_SYSVIPC_TYPE_SHM) {
		ret = -EINVAL;
		goto out_put;
	}

	ret = svipc_shm_ensure_file(res);
	if (ret)
		goto out_put;

	if (!(shmflg & SHM_RDONLY))
		prot |= PROT_WRITE;
	if (shmflg & SHM_EXEC)
		prot |= PROT_EXEC;

	att = kzalloc(sizeof(*att), GFP_KERNEL);
	if (!att) {
		ret = -ENOMEM;
		goto out_put;
	}

	/*
	 * shmat(2)'s @shmaddr is only ever a placement *hint* unless SHM_REMAP
	 * is also given (see man shmat(2)): with a non-NULL hint but no
	 * SHM_REMAP, real do_shmat() still lets the kernel pick a different
	 * address if the hint's range is already occupied, rather than
	 * unconditionally forcing (and clobbering) exactly that address the
	 * way MAP_FIXED would. Only add MAP_FIXED when SHM_REMAP was
	 * explicitly requested, matching that semantic.
	 */
	get_file(res->shm_file);
	addr = svipc_shm_vm_mmap(res->shm_file, (unsigned long)ushmaddr,
				  res->size, prot,
				  (ushmaddr && (shmflg & SHM_REMAP)) ?
					  MAP_SHARED | MAP_FIXED : MAP_SHARED,
				  0);
	fput(res->shm_file);
	if (IS_ERR_VALUE(addr)) {
		kfree(att);
		ret = (long)addr;
		goto out_put;
	}

	att->tgid = task_tgid_nr(current);
	att->addr = addr;
	att->len = res->size;
	att->res = res; /* transfers our svipc_get() reference to the attach */

	mutex_lock(&svipc_shm_attach_lock);
	list_add(&att->node, &svipc_shm_attach_list);
	mutex_unlock(&svipc_shm_attach_lock);

	*raddr = addr;
	return 0;

out_put:
	svipc_put(res);
	return ret;
}

long svipc_sys_shmdt(const void __user *ushmaddr)
{
	struct svipc_shm_attach *att, *tmp;
	unsigned long addr = (unsigned long)ushmaddr;
	pid_t tgid = task_tgid_nr(current);
	int ret;

	svipc_shm_reap_dead();

	mutex_lock(&svipc_shm_attach_lock);
	list_for_each_entry_safe(att, tmp, &svipc_shm_attach_list, node) {
		if (att->tgid == tgid && att->addr == addr) {
			list_del(&att->node);
			mutex_unlock(&svipc_shm_attach_lock);

			ret = svipc_shm_vm_munmap(att->addr, att->len);
			svipc_put(att->res); /* drops the attach's own reference */
			kfree(att);
			return ret;
		}
	}
	mutex_unlock(&svipc_shm_attach_lock);
	return -EINVAL;
}
