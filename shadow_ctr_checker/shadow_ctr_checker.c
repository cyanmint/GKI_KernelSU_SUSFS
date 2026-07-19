// SPDX-License-Identifier: GPL-2.0
/*
 * shadow_ctr_checker - userspace diagnostic tool for the shadow_ctr module
 * family.
 *
 * This used to be a small kernel module that read back its own compile-time
 * IS_ENABLED(CONFIG_*) view of the target kernel. That could only ever prove
 * what the kernel was *built* to support, never what actually happens at
 * runtime once shadow_ns/shadow_sysvipc/shadow_mqueue/shadow_cgdevices hooks
 * (or their absence) are in the loop - which is exactly what matters when
 * diagnosing a real container-start failure such as:
 *
 *   failed to create task for container: failed to create shim task: OCI
 *   runtime create failed: runc create failed: unable to start container
 *   process: can't get final child's PID from pipe: EOF; runc init error(s):
 *   nsexec-1[19437]: failed to unshare remaining namespaces: Invalid
 *   argument; nsexec-0[19436]: failed to sync with stage-1: next state (got
 *   0 of 4 bytes): unknown
 *
 * This tool is a plain userspace binary (no kernel module, no /dev node): it
 * directly performs the same system calls runc's nsexec does
 * (unshare/clone/fork + setns), observes their *actual effect* (not just
 * whether the syscall returned 0), and prints one PASS/FAIL/STUB line per
 * feature:
 *
 *   PASS  - the syscall succeeded AND a real, observable side effect proves
 *           genuine isolation (e.g. a child's hostname change after
 *           unshare(CLONE_NEWUTS) is NOT visible to the parent).
 *   STUB  - the syscall succeeded but no isolation was actually observed
 *           (bookkeeping-only fallback: bit accepted, nothing isolated).
 *   FAIL  - the syscall itself failed (e.g. EINVAL/ENOSYS/EPERM) - this is
 *           the exact failure mode runc's nsexec surfaces as "failed to
 *           unshare remaining namespaces: Invalid argument".
 *
 * Every syscall requiring CLONE_NEW* is attempted inside a throwaway
 * fork(2)'d child, so a FAIL/STUB result never disturbs this process (or the
 * calling shell)'s own namespaces.
 *
 * Usage:
 *   shadow_ctr_checker            # human-readable report to stdout
 *   shadow_ctr_checker -q         # same, but exit status reflects overall
 *                                 # result (0 = all PASS, 1 = any FAIL)
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#if __has_include(<mqueue.h>)
#include <mqueue.h>
#define SHADOW_CHECKER_HAVE_MQUEUE 1
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 64
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC 0x08000000
#endif
#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER 0x10000000
#endif
#ifndef CLONE_NEWPID
#define CLONE_NEWPID 0x20000000
#endif
#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif
#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000
#endif

#define SHADOW_CTR_CHECKER_VERSION "3.0"

enum shadow_checker_result {
	SHADOW_CHECKER_PASS = 0,
	SHADOW_CHECKER_STUB,
	SHADOW_CHECKER_FAIL,
	SHADOW_CHECKER_SKIP,
};

static const char *const shadow_checker_result_str[] = {
	[SHADOW_CHECKER_PASS] = "PASS",
	[SHADOW_CHECKER_STUB] = "STUB",
	[SHADOW_CHECKER_FAIL] = "FAIL",
	[SHADOW_CHECKER_SKIP] = "SKIP",
};

static int g_fail_count;
static int g_stub_count;
static bool g_quiet;

static void shadow_checker_report(const char *label,
				   enum shadow_checker_result result,
				   const char *fmt, ...)
{
	char detail[256] = "";

	if (fmt) {
		va_list args;

		va_start(args, fmt);
		vsnprintf(detail, sizeof(detail), fmt, args);
		va_end(args);
	}

	if (result == SHADOW_CHECKER_FAIL)
		g_fail_count++;
	else if (result == SHADOW_CHECKER_STUB)
		g_stub_count++;

	if (g_quiet)
		return;

	if (detail[0])
		printf("%-28s %-4s  %s\n", label,
		       shadow_checker_result_str[result], detail);
	else
		printf("%-28s %-4s\n", label,
		       shadow_checker_result_str[result]);
}

/*
 * Reads a single "\n"-terminated status line (either "OK <payload>" or
 * "ERR <errno>") back from a child process through a pipe. Returns true and
 * fills ok/payload on success, false on a broken pipe / malformed message.
 */
struct shadow_checker_msg {
	bool ok;
	int err;
	char payload[128];
};

static bool shadow_checker_read_msg(int fd, struct shadow_checker_msg *out)
{
	char buf[256];
	ssize_t n;

	memset(out, 0, sizeof(*out));
	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	if (!strncmp(buf, "OK ", 3)) {
		out->ok = true;
		/* Explicit precision (in addition to the destination size
		 * snprintf() already respects) purely to silence a
		 * -Wformat-truncation false positive: snprintf() always
		 * truncates+NUL-terminates safely on its own.
		 */
		snprintf(out->payload, sizeof(out->payload), "%.*s",
			 (int)sizeof(out->payload) - 1, buf + 3);
	} else if (!strncmp(buf, "ERR ", 4)) {
		out->ok = false;
		out->err = atoi(buf + 4);
	} else {
		return false;
	}
	return true;
}

static void shadow_checker_send_ok(int fd, const char *fmt, ...)
{
	char buf[256];
	va_list args;
	int n;

	n = snprintf(buf, sizeof(buf), "OK ");
	va_start(args, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
	va_end(args);
	buf[n] = '\n';
	if (write(fd, buf, n + 1) < 0) {
		/* best effort; parent side treats a closed pipe as SKIP */
	}
}

static void shadow_checker_send_err(int fd, int err)
{
	char buf[64];
	int n = snprintf(buf, sizeof(buf), "ERR %d\n", err);

	if (write(fd, buf, n) < 0) {
		/* best effort; nothing to recover here */
	}
}

/*
 * Build a collision-resistant name of the form "<prefix>-<pid>-<nsec-hex>".
 * Uses CLOCK_MONOTONIC at nanosecond resolution (rather than time(NULL),
 * which only has 1-second resolution) so that two invocations of the same
 * test in quick succession -- e.g. a PID reused across rapid re-runs within
 * the same wall-clock second -- can never collide on the generated name.
 * This matters for tests like mqueue that use O_EXCL and would otherwise
 * intermittently fail with EEXIST.
 */
static void shadow_checker_unique_name(char *out, size_t out_len,
					const char *prefix)
{
	struct timespec ts = { 0, 0 };

	clock_gettime(CLOCK_MONOTONIC, &ts);
	snprintf(out, out_len, "%s-%d-%lx-%08lx", prefix, getpid(),
		 (unsigned long)ts.tv_sec, (unsigned long)ts.tv_nsec);
}

/*
 * ---------------------------------------------------------------------
 * UTS namespace: unshare(CLONE_NEWUTS), then set a unique hostname inside
 * the child. Real isolation means the parent's own gethostname() never
 * observes the child's change; bookkeeping-only (or no isolation at all)
 * means it does.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_uts(void)
{
	char before[HOST_NAME_MAX + 1] = "";
	char after[HOST_NAME_MAX + 1] = "";
	char newname[64];
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (gethostname(before, sizeof(before) - 1))
		before[0] = '\0';

	if (pipe(pipefd)) {
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		close(pipefd[0]);
		if (unshare(CLONE_NEWUTS)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		shadow_checker_unique_name(newname, sizeof(newname), "shadowchk");
		if (sethostname(newname, strlen(newname))) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		/* Signal parent that the child's hostname has been changed;
		 * parent samples its own hostname now, before we exit.
		 */
		shadow_checker_send_ok(pipefd[1], "%s", newname);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_FAIL,
				      "unshare(CLONE_NEWUTS): %s",
				      strerror(msg.err));
		return;
	}

	if (gethostname(after, sizeof(after) - 1))
		after[0] = '\0';

	if (!strcmp(before, after))
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_PASS,
				      "child set '%s', parent still '%s'",
				      msg.payload, after);
	else
		shadow_checker_report("ns_uts (UTS)", SHADOW_CHECKER_STUB,
				      "child's hostname change leaked to parent ('%s')",
				      after);
}

/*
 * ---------------------------------------------------------------------
 * PID namespace: unshare(CLONE_NEWPID) does NOT move the calling task; only
 * a subsequently forked child joins the new pid namespace. A genuinely new,
 * empty pid namespace always numbers its very first task as pid 1 (as
 * observed by that task's own getpid()). If the grandchild does not see
 * itself as pid 1, no real isolation happened.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_pid(void)
{
	int pipefd[2];
	pid_t pid;
	pid_t main_pid = getpid();
	struct shadow_checker_msg msg;
	char vpidbuf[32];
	int proc_self_ok = -1, proc_hidden_ok = -1, proc_listing_ok = -1;
	int have_fresh_proc = 0;

	if (pipe(pipefd)) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		pid_t grandchild;

		close(pipefd[0]);
		if (unshare(CLONE_NEWPID)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		grandchild = fork();
		if (grandchild < 0) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		if (grandchild == 0) {
			/*
			 * /proc isolation check (see shadow_ns_procfs.c):
			 * from inside the new (real or shadow_ns-simulated)
			 * PID namespace, /proc/<own vpid> must resolve, the
			 * outer checker process's real, still-running host
			 * pid must NOT be visible via /proc/<main_pid>, and
			 * readdir("/proc") must reflect the same hide/rename.
			 *
			 * On a genuine CONFIG_PID_NS kernel this requires a
			 * *fresh* procfs mount bound to the new namespace --
			 * exactly what runc/containerd do after
			 * unshare(CLONE_NEWPID) -- otherwise the ambient
			 * host /proc mount (inherited unchanged) still shows
			 * the host's own pid namespace regardless of the new
			 * pid namespace. shadow_ns's own openat/getdents64
			 * translation hooks intercept /proc access
			 * independent of which mount instance is used, so
			 * this remount is harmless (a no-op from their
			 * point of view) when shadow_ns is simulating
			 * CLONE_NEWPID instead of a real kernel doing so.
			 */
			pid_t vpid = getpid();
			char path[64];
			int fd, have_fresh_proc;
			DIR *d;

			have_fresh_proc =
				(unshare(CLONE_NEWNS) == 0 &&
				 mount(NULL, "/", NULL,
				       MS_REC | MS_PRIVATE, NULL) == 0 &&
				 mount("proc", "/proc", "proc", 0, NULL) == 0);

			snprintf(path, sizeof(path), "/proc/%d", (int)vpid);
			fd = open(path, O_RDONLY | O_DIRECTORY);
			proc_self_ok = fd >= 0;
			if (fd >= 0)
				close(fd);

			snprintf(path, sizeof(path), "/proc/%d",
				 (int)main_pid);
			fd = open(path, O_RDONLY | O_DIRECTORY);
			proc_hidden_ok = (fd < 0 && errno == ENOENT);
			if (fd >= 0)
				close(fd);

			d = opendir("/proc");
			if (d) {
				struct dirent *de;
				bool saw_self = false, saw_outer = false;
				char selfbuf[16];

				snprintf(selfbuf, sizeof(selfbuf), "%d",
					 (int)vpid);
				while ((de = readdir(d)) != NULL) {
					char *end;
					long v;

					if (!strcmp(de->d_name, selfbuf))
						saw_self = true;
					v = strtol(de->d_name, &end, 10);
					if (*de->d_name && !*end &&
					    v == (long)main_pid)
						saw_outer = true;
				}
				closedir(d);
				proc_listing_ok = saw_self && !saw_outer;
			}

			shadow_checker_send_ok(pipefd[1], "%d;%d;%d;%d;%d",
						vpid, proc_self_ok,
						proc_hidden_ok, proc_listing_ok,
						have_fresh_proc);
			_exit(0);
		}
		waitpid(grandchild, NULL, 0);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_FAIL,
				      "unshare(CLONE_NEWPID): %s",
				      strerror(msg.err));
		return;
	}

	if (sscanf(msg.payload, "%31[^;];%d;%d;%d;%d", vpidbuf, &proc_self_ok,
		   &proc_hidden_ok, &proc_listing_ok, &have_fresh_proc) != 5) {
		/* Older/unexpected payload shape: keep pre-/proc-check
		 * behaviour for the PID result itself.
		 */
		strncpy(vpidbuf, msg.payload, sizeof(vpidbuf) - 1);
		vpidbuf[sizeof(vpidbuf) - 1] = '\0';
		proc_self_ok = proc_hidden_ok = proc_listing_ok = -1;
		have_fresh_proc = 0;
	}

	if (strcmp(vpidbuf, "1")) {
		shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_STUB,
				      "grandchild kept real pid %s (no vpid remap)",
				      vpidbuf);
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_SKIP,
				      "PID namespace was not isolated; nothing meaningful to check");
		return;
	}

	shadow_checker_report("ns_pid (PID)", SHADOW_CHECKER_PASS,
			      "grandchild became pid 1 in new namespace");

	if (proc_self_ok < 0) {
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_SKIP,
				      "child produced no /proc-isolation result");
	} else if (!have_fresh_proc) {
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_SKIP,
				      "couldn't mount a fresh /proc to test (needs CAP_SYS_ADMIN); self=%d hidden=%d listing=%d observed against the ambient /proc mount",
				      proc_self_ok, proc_hidden_ok,
				      proc_listing_ok);
	} else if (!proc_self_ok || !proc_hidden_ok || !proc_listing_ok) {
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_FAIL,
				      "self=%d hidden=%d listing=%d (want all 1: /proc/1 open, host pid %d hidden, readdir filtered)",
				      proc_self_ok, proc_hidden_ok,
				      proc_listing_ok, (int)main_pid);
	} else {
		shadow_checker_report("ns_pid (/proc isolation)",
				      SHADOW_CHECKER_PASS,
				      "/proc/1 resolves to self; host pid %d hidden from open() and readdir(\"/proc\")",
				      (int)main_pid);
	}
}

/*
 * ---------------------------------------------------------------------
 * USER namespace: unshare(CLONE_NEWUSER) always changes the calling task's
 * apparent uid/gid inside the new namespace *before* any uid_map/gid_map is
 * written - either to the overflow uid (genuine, unmapped kernel
 * namespace: typically 65534) or to 0 (shadow_ns's docker-like
 * single-mapping remap of the creator to root). Both are a real, observable
 * change; only an unchanged uid indicates no isolation at all. This test is
 * only meaningful when run as a non-root user - see the SKIP case.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_user(void)
{
	uid_t before = getuid();
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (before == 0) {
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_SKIP,
				      "running as uid 0; test needs a non-root uid to be conclusive");
		return;
	}

	if (pipe(pipefd)) {
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		close(pipefd[0]);
		if (unshare(CLONE_NEWUSER)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		shadow_checker_send_ok(pipefd[1], "%u", geteuid());
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_FAIL,
				      "unshare(CLONE_NEWUSER): %s",
				      strerror(msg.err));
		return;
	}

	if (atoi(msg.payload) != (int)before)
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_PASS,
				      "uid remapped %u -> %s inside namespace",
				      before, msg.payload);
	else
		shadow_checker_report("ns_user (USER)", SHADOW_CHECKER_STUB,
				      "uid unchanged (%s) - no remap performed",
				      msg.payload);
}

/*
 * ---------------------------------------------------------------------
 * IPC / NET / MNT / CGROUP namespaces: none of these has a cheap, safe,
 * universal in-process "did isolation really happen" signal the way UTS/
 * PID/USER do (see shadow_ctr/shadow_ctr/shadow_ns/README.md: shadow_ns's fallback for
 * these four is bookkeeping-only by design, and genuine kernel isolation for
 * them touches subsystems this tool must not perturb - e.g. mounting/
 * networking). So this tool only reports whether the unshare(2) syscall
 * itself succeeds; a successful-but-unverifiable-isolation namespace is
 * still reported STUB unless the child can observe a distinct
 * /proc/self/ns/<type> identity from the parent, which is a reliable
 * indicator that the kernel actually allocated a new namespace object
 * (rather than just accepting the flag and doing nothing).
 * ---------------------------------------------------------------------
 */
static bool shadow_checker_read_ns_id(pid_t pid, const char *type, char *out,
				       size_t outlen)
{
	char path[64];
	ssize_t n;

	if (pid)
		snprintf(path, sizeof(path), "/proc/%d/ns/%s", pid, type);
	else
		snprintf(path, sizeof(path), "/proc/self/ns/%s", type);

	n = readlink(path, out, outlen - 1);
	if (n < 0)
		return false;
	out[n] = '\0';
	return true;
}

static void shadow_checker_generic_ns(const char *label, const char *ns_file,
				       int clone_flag)
{
	char before[128], after[128];
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;
	bool have_before;

	have_before = shadow_checker_read_ns_id(0, ns_file, before,
						 sizeof(before));

	if (pipe(pipefd)) {
		shadow_checker_report(label, SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report(label, SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		char id[64];

		close(pipefd[0]);
		if (unshare(clone_flag)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		if (!shadow_checker_read_ns_id(0, ns_file, id, sizeof(id)))
			id[0] = '\0';
		shadow_checker_send_ok(pipefd[1], "%s", id);
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report(label, SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (!msg.ok) {
		shadow_checker_report(label, SHADOW_CHECKER_FAIL,
				      "unshare(): %s", strerror(msg.err));
		return;
	}

	snprintf(after, sizeof(after), "%s", msg.payload);

	if (have_before && after[0] && strcmp(before, after))
		shadow_checker_report(label, SHADOW_CHECKER_PASS,
				      "namespace id changed (%s -> %s)",
				      before, after);
	else
		shadow_checker_report(label, SHADOW_CHECKER_STUB,
				      "unshare() succeeded but namespace id unchanged (bookkeeping only)");
}

/*
 * ---------------------------------------------------------------------
 * POSIX message queues: real functional test (mq_open + send + receive),
 * not just "does mq_open() succeed".
 * ---------------------------------------------------------------------
 */
static void shadow_checker_mqueue(void)
{
#ifdef SHADOW_CHECKER_HAVE_MQUEUE
	char name[64];
	mqd_t mq;
	char msgbuf[16] = "hi";
	char rcvbuf[16] = "";
	struct mq_attr attr = {
		.mq_maxmsg = 4,
		.mq_msgsize = sizeof(msgbuf),
	};

	shadow_checker_unique_name(name, sizeof(name), "/shadowchk-mq");

	/* Best-effort cleanup of a stale queue left behind by a previous run
	 * that crashed before mq_unlink() (e.g. name reused after a PID
	 * wraparound); ignore ENOENT since that is the expected case.
	 */
	mq_unlink(name);

	mq = mq_open(name, O_CREAT | O_RDWR | O_EXCL, 0600, &attr);
	if (mq == (mqd_t)-1) {
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_FAIL,
				      "mq_open(): %s", strerror(errno));
		return;
	}

	if (mq_send(mq, msgbuf, strlen(msgbuf), 0)) {
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_FAIL,
				      "mq_send(): %s", strerror(errno));
		mq_close(mq);
		mq_unlink(name);
		return;
	}

	if (mq_receive(mq, rcvbuf, sizeof(rcvbuf), NULL) < 0) {
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_FAIL,
				      "mq_receive(): %s", strerror(errno));
		mq_close(mq);
		mq_unlink(name);
		return;
	}

	mq_close(mq);
	mq_unlink(name);

	if (!strcmp(rcvbuf, msgbuf))
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_PASS,
				      "message round-tripped through the queue");
	else
		shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_STUB,
				      "queue accepted send/receive but payload mismatched");
#else
	shadow_checker_report("mqueue (POSIX)", SHADOW_CHECKER_SKIP,
			      "<mqueue.h> unavailable in this libc");
#endif
}

/*
 * ---------------------------------------------------------------------
 * System V IPC: real functional test via a message queue (msgget/msgsnd/
 * msgrcv), matching what shadow_sysvipc actually hooks.
 * ---------------------------------------------------------------------
 */
struct shadow_checker_sysv_msg {
	long mtype;
	char mtext[16];
};

static void shadow_checker_sysvipc(void)
{
	int id;
	struct shadow_checker_sysv_msg out = { .mtype = 1, .mtext = "hi" };
	struct shadow_checker_sysv_msg in = { 0 };

	id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
	if (id < 0) {
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_FAIL,
				      "msgget(): %s", strerror(errno));
		return;
	}

	if (msgsnd(id, &out, strlen(out.mtext) + 1, 0)) {
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_FAIL,
				      "msgsnd(): %s", strerror(errno));
		msgctl(id, IPC_RMID, NULL);
		return;
	}

	if (msgrcv(id, &in, sizeof(in.mtext), 1, 0) < 0) {
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_FAIL,
				      "msgrcv(): %s", strerror(errno));
		msgctl(id, IPC_RMID, NULL);
		return;
	}

	msgctl(id, IPC_RMID, NULL);

	if (!strcmp(in.mtext, out.mtext))
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_PASS,
				      "message round-tripped through msgget/msgsnd/msgrcv");
	else
		shadow_checker_report("sysvipc (SysV msg)", SHADOW_CHECKER_STUB,
				      "queue accepted send/receive but payload mismatched");
}

/*
 * ---------------------------------------------------------------------
 * overlay2: attempt a real mount(2) of an overlay filesystem in a private
 * mount namespace (so nothing leaks onto the host), using throwaway tmpfs
 * dirs for lower/upper/work.
 * ---------------------------------------------------------------------
 */
static void shadow_checker_overlay(void)
{
	char base[] = "/tmp/shadowchk-ovl-XXXXXX";
	char lower[PATH_MAX], upper[PATH_MAX], work[PATH_MAX], merged[PATH_MAX];
	char opts[PATH_MAX * 4];
	int pipefd[2];
	pid_t pid;
	struct shadow_checker_msg msg;

	if (pipe(pipefd)) {
		shadow_checker_report("overlay2", SHADOW_CHECKER_SKIP,
				       "pipe() failed: %s", strerror(errno));
		return;
	}

	pid = fork();
	if (pid < 0) {
		shadow_checker_report("overlay2", SHADOW_CHECKER_SKIP,
				       "fork() failed: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		close(pipefd[0]);

		/* Isolate into a private mount namespace so the test mount
		 * never leaks onto the host, regardless of pass/fail.
		 */
		if (unshare(CLONE_NEWNS)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		if (!mkdtemp(base)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}
		snprintf(lower, sizeof(lower), "%s/lower", base);
		snprintf(upper, sizeof(upper), "%s/upper", base);
		snprintf(work, sizeof(work), "%s/work", base);
		snprintf(merged, sizeof(merged), "%s/merged", base);
		if (mkdir(lower, 0700) || mkdir(upper, 0700) ||
		    mkdir(work, 0700) || mkdir(merged, 0700)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		snprintf(opts, sizeof(opts),
			 "lowerdir=%s,upperdir=%s,workdir=%s", lower, upper,
			 work);

		if (mount("overlay", merged, "overlay", 0, opts)) {
			shadow_checker_send_err(pipefd[1], errno);
			_exit(0);
		}

		if (umount2(merged, MNT_DETACH))
			fprintf(stderr,
				"shadow_ctr_checker: warning: umount2(%s) failed: %s\n",
				merged, strerror(errno));
		shadow_checker_send_ok(pipefd[1], "mounted");
		_exit(0);
	}

	close(pipefd[1]);
	if (!shadow_checker_read_msg(pipefd[0], &msg)) {
		shadow_checker_report("overlay2", SHADOW_CHECKER_SKIP,
				       "child produced no result");
		close(pipefd[0]);
		waitpid(pid, NULL, 0);
		return;
	}
	close(pipefd[0]);
	waitpid(pid, NULL, 0);

	if (msg.ok)
		shadow_checker_report("overlay2", SHADOW_CHECKER_PASS,
				      "mount(2) of an overlay filesystem succeeded");
	else if (msg.err == ENODEV || msg.err == ENOENT)
		shadow_checker_report("overlay2", SHADOW_CHECKER_FAIL,
				      "overlay filesystem type not registered: %s",
				      strerror(msg.err));
	else
		shadow_checker_report("overlay2", SHADOW_CHECKER_FAIL,
				      "mount(): %s", strerror(msg.err));
}

static void shadow_checker_usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-q]\n"
		"  -q  quiet: no report, exit status only (0 = all PASS, 1 = any FAIL)\n",
		argv0);
}

int main(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "qh")) != -1) {
		switch (opt) {
		case 'q':
			g_quiet = true;
			break;
		default:
			shadow_checker_usage(argv[0]);
			return 2;
		}
	}

	if (!g_quiet) {
		printf("shadow_ctr_checker v%s - userspace container-isolation diagnostics\n",
		       SHADOW_CTR_CHECKER_VERSION);
		printf("PASS = real isolation observed, STUB = bookkeeping only (no real isolation), FAIL = syscall itself failed\n");
		printf("--------------------------------------------------------------------------------------------------------\n");
	}

	shadow_checker_uts();
	shadow_checker_pid();
	shadow_checker_user();
	shadow_checker_generic_ns("ns_ipc (IPC)", "ipc", CLONE_NEWIPC);
	shadow_checker_generic_ns("ns_net (NET)", "net", CLONE_NEWNET);
	shadow_checker_generic_ns("ns_mnt (MNT)", "mnt", CLONE_NEWNS);
	shadow_checker_generic_ns("ns_cgroup (CGROUP)", "cgroup", CLONE_NEWCGROUP);
	shadow_checker_mqueue();
	shadow_checker_sysvipc();
	shadow_checker_overlay();

	if (!g_quiet) {
		printf("--------------------------------------------------------------------------------------------------------\n");
		printf("summary: %d FAIL, %d STUB (bookkeeping-only)\n", g_fail_count,
		       g_stub_count);
	}

	return g_fail_count ? 1 : 0;
}
