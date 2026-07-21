#!/busybox sh
# Merged host-launcher + checker-mode + stage-1/stage-2 init script. This
# single file plays four different roles, dispatched purely by PID/argv[0]/
# $1 -- no separate files are used to select the mode:
#
#   1. Host-side QEMU launcher ("qemu_test.sh" mode), when $$ (the running
#      shell's own PID) is not 1 and $1 is not "-t" -- i.e. whenever this
#      script is not acting as some kernel's PID 1, and is not being asked
#      to run checker mode (see role 2 below). Boots a single kernel under
#      QEMU against a given image1.ext4 (and, optionally, an injected
#      ramdisk), for use in both build-lkm4ctr.yml and ad-hoc manual
#      debugging:
#
#        bash qemu_test.sh <image1.ext4 path|-l> <kernel path> <init path> [ramdisk path]
#
#      If <ramdisk path> is given, boots with "-initrd <ramdisk path>" and
#      "rdinit=<init path>" (the injected ramdisk owns /, so the kernel is
#      told which program on *that* ramdisk to run as init). If omitted,
#      boots without "-initrd" at all, and "root=/dev/nvme0n1
#      init=<init path>" instead (image1.ext4 -- /dev/nvme0n1 -- is used
#      directly as the root filesystem, so <init path> must exist there).
#
#      If <image1.ext4 path> is literally "-l" ("light mode"), no
#      image1.ext4/nvme drive is attached at all -- <ramdisk path> is then
#      required, and the kernel boots ramdisk-only (rdinit=<init path>,
#      no root= at all). This is for exercising the module against a bare
#      busybox environment with no real Android userland/dockerd
#      available, e.g. when no image1.ext4 has been built/downloaded.
#
#      IMPORTANT: this file's shebang is "#!/busybox sh" (needed for roles
#      2-4 below, where the kernel/switch_root/self-invocation exec it
#      directly and only /busybox is guaranteed to exist). That shebang is
#      almost certainly *not* usable on a plain Ubuntu host (no /busybox
#      binary), so this script must always be invoked explicitly as `bash
#      qemu_test.sh ...` on the host -- never `./qemu_test.sh ...`, which
#      would honor the shebang and try (and typically fail) to exec
#      /busybox.
#
#   2. Checker mode ("$1" == "-t"), run as an ordinary (non-PID-1) child
#      process, always invoked as:
#
#        <some sh> <this script's own path> -t <checker path> <module path>
#
#      This extracts every actual lkm4ctr test step (previously
#      duplicated inline in stage 2 below) into one shared routine used by
#      both the full ("heavy", image1.ext4 + dockerd) and "light"
#      (ramdisk-only, no nvme/docker) boot paths: lkm4ctr_checker
#      (pre-insmod), insmod, lkm4ctr_checker (post-insmod), mount the
#      "lkm4ctr" diagfs, read every control/status/hooks/namespaces/log/resource file, a
#      "hot upgrade" pass (unload then reload one submodule via its diagfs
#      control file while lkm4ctr.ko stays resident), force-deactivating
#      every submodule, triggering the global self-unload path via
#      diagfs/global/control, and finally falling back to a manual
#      umount+rmmod if the global unload path itself did not fully unload the module.
#
#   3. Stage-1 init (PID 1, argv[0]/$0 == "init"): this is the role played
#      when the script is baked into the injected (busybox) ramdisk as
#      /init and the kernel starts it as rdinit=/init. It:
#        a. populates /dev via mdev -- the QEMU test kernels have no
#           devtmpfs support, so /dev must be populated the old way: mount
#           sysfs, then `mdev -s` coldplugs every device node (console,
#           kmsg, nvme if present...) from /sys.
#        b. checks whether /dev/nvme0n1 exists (i.e. whether the host
#           launcher attached an image1.ext4 drive at all -- absent in
#           "light mode", see role 1 above) and mounts it at /newroot.
#        c. If nvme *is* available (the normal/"heavy" boot path):
#             - copies /lkm4ctr.ko + /lkm4ctr_checker, baked into this
#               same ramdisk, onto the new root.
#             - copies this very script onto the new root as /second_init
#               (`cat /init` works because /init is this script's own
#               path in the initramfs).
#             - busybox switch_root's into /newroot and execs
#               /second_init -- since its argv[0] is now "/second_init"
#               rather than "init", it runs the stage-2 body (role 4)
#               instead of stage-1 again.
#        d. If nvme is *not* available ("light mode"): there is no real
#           root to switch into and no dockerd/Android userland to
#           exercise at all, so instead of stage 2 this copies itself to
#           /tester and directly invokes checker mode (role 2) against
#           the ramdisk's own /lkm4ctr.ko + /lkm4ctr_checker under plain
#           busybox sh, then falls straight through to the shared
#           reboot/poweroff tail below.
#
#   4. Stage-2 init (PID 1, argv[0]/$0 != "init"): out of the real root,
#      image1.ext4, as /second_init. Populates /dev again (same reasoning
#      as stage 1 -- switch_root does not preserve the initramfs's
#      un-mounted /dev contents), sources /do-mounts.sh, copies itself to
#      /tester and invokes checker mode (role 2) against image1.ext4's
#      target userland shell, then starts a real dockerd, imports+runs
#      the alpine/ubuntu tarballs already baked into image1.ext4, sources
#      /do-umounts.sh, and falls through to the shared reboot/poweroff
#      tail below.
#
# Both stage 1 (light mode) and stage 2 end by triggering an `echo o >
# /proc/sysrq-trigger` poweroff and looping briefly waiting for it to take
# effect -- this is the shared "reboot" tail every PID-1 role ends with.

# ==========================================================================
# checker mode routine, shared by stage-1 (light mode) and stage-2 (heavy
# mode): every actual lkm4ctr functional test (insmod, diagfs, global unload)
# lives here exactly once.
# ==========================================================================
lkm4ctr_run_checker_mode() {
	CHECKER="$1"
	MODULE="$2"
	MNT=/lkm4ctr_diagfs
	# generous enough for the global unload worker's own auto-umount +
	# module_refcount()-drain polling (see lkm4ctr_diagfs.c) to finish on a
	# loaded QEMU VM, without hanging the whole boot test indefinitely if
	# it never does.
	SAFE_UNLOAD_TIMEOUT_SEC=30

	echo "=== LKM4CTR_QEMU_TEST: lkm4ctr_checker (pre-insmod) ==="
	"$CHECKER"

	echo "=== LKM4CTR_QEMU_TEST: inserting merged module ==="
	insmod "$MODULE"
	dmesg | tail -n 80

	echo "=== LKM4CTR_QEMU_TEST: lkm4ctr_checker (post-insmod) ==="
	"$CHECKER"

	echo "=== LKM4CTR_QEMU_TEST: mounting lkm4ctr diagfs ==="
	mkdir -p "$MNT"
	mount -t lkm4ctr diag "$MNT"
	echo "mount -t lkm4ctr diag $MNT -> $?"

	echo "=== LKM4CTR_QEMU_TEST: diagfs control/status/hooks/namespaces/log ==="
	for m in hijack ns sysvipc mqueue cgroupdevices; do
		echo "--- $m/control ---"
		cat "$MNT/$m/control"
		echo "--- $m/status ---"
		cat "$MNT/$m/status"
		if [ -f "$MNT/$m/hooks" ]; then
			echo "--- $m/hooks ---"
			cat "$MNT/$m/hooks"
		fi
		if [ -f "$MNT/$m/namespaces" ]; then
			echo "--- $m/namespaces ---"
			cat "$MNT/$m/namespaces"
		fi
		if [ -f "$MNT/$m/msg" ]; then
			echo "--- $m/msg ---"
			cat "$MNT/$m/msg"
		fi
		if [ -f "$MNT/$m/functions" ]; then
			echo "--- $m/functions ---"
			cat "$MNT/$m/functions"
		fi
		echo "--- $m/log (tail) ---"
		cat "$MNT/$m/log" | tail -n 10
	done
	echo "--- global/resources ---"
	cat "$MNT/global/resources"
	echo "--- global/log (tail) ---"
	cat "$MNT/global/log" | tail -n 20
	echo "--- ns/pid/namespaces ---"
	cat "$MNT/ns/pid/namespaces"

	echo "=== LKM4CTR_QEMU_TEST: diagfs hot upgrade (unload/reload shadow_ns hooks) ==="
	echo unload > "$MNT/ns/control"
	echo "post-unload status: $(cat "$MNT/ns/status")"
	echo load > "$MNT/ns/control"
	echo "post-reload status: $(cat "$MNT/ns/status")"

	echo "=== LKM4CTR_QEMU_TEST: deactivating all submodules via diagfs ==="
	for m in ns sysvipc mqueue cgroupdevices; do
		echo unload > "$MNT/$m/control"
		echo "$m/status after unload: $(cat "$MNT/$m/status")"
	done

	echo "=== LKM4CTR_QEMU_TEST: global unload via diagfs ==="
	cat "$MNT/global/control"
	echo unload > "$MNT/global/control"
	for _ in $(seq 1 "$SAFE_UNLOAD_TIMEOUT_SEC"); do
		grep -q '^lkm4ctr ' /proc/modules || break
		sleep 1
	done
	if grep -q '^lkm4ctr ' /proc/modules; then
		echo "LKM4CTR_QEMU_TEST: global unload FAILED, module still loaded"
	else
		echo "LKM4CTR_QEMU_TEST: global unload OK, module unloaded itself"
	fi
	dmesg | tail -n 40

	# global unload auto-unmounts every active diagfs mount itself before it
	# starts waiting for module_refcount() to drain (see
	# lkm4ctr_auto_umount_diagfs() in lkm4ctr_diagfs.c), so $MNT should
	# already be gone; this is just a last-resort fallback in case
	# the global unload path could not fully unload the module (e.g. still busy for
	# some other reason), to leave the system in a clean state either way.
	if grep -q '^lkm4ctr ' /proc/modules; then
		echo "=== LKM4CTR_QEMU_TEST: global unload did not remove the module, forcing umount+rmmod ==="
		umount "$MNT" 2>/dev/null
		rmmod "$MODULE"
		echo "rmmod -> $?"
	fi

	echo "=== LKM4CTR_QEMU_TEST: checker mode DONE ==="
}

# ==========================================================================
# shared reboot/poweroff tail, run at the end of every PID-1 role
# (light-mode stage 1, and stage 2) once testing is done.
# ==========================================================================
lkm4ctr_do_reboot() {
	# $1: "heavy" (real image1.ext4 root, bionic userland present) or
	# "light" (busybox-only initramfs, no real root to remount).
	if [ "$1" = "heavy" ]; then
		/system/bin/mount -o remount,ro / 2>/dev/null
	fi
	echo o > /proc/sysrq-trigger
	for i in 1 2 3 4 5; do
		echo "$i"
		sleep "$i"
	done
	if [ "$1" = "heavy" ]; then
		exec env -i /system/bin/sh
	else
		exec busybox sh
	fi
}

if [ "$$" != "1" ]; then
	# ==================================================================
	# mode 1/2: host-side QEMU launcher, or checker mode ("-t")
	# ==================================================================
	set -uo pipefail
	# Note: intentionally not "set -e" -- QEMU's exit status (including a
	# timeout/KILL) is captured explicitly below so the console log is
	# always printed and this script's own exit status still reflects
	# QEMU's, rather than the shell aborting immediately on a nonzero
	# exit.

	if [ "${1:-}" = "-t" ]; then
		# checker mode: <sh> $0 -t <checker path> <module path>
		if [ "$#" -ne 3 ]; then
			echo "usage: $0 -t <checker path> <module path>" >&2
			exit 2
		fi
		lkm4ctr_run_checker_mode "$2" "$3"
		exit 0
	fi

	if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
		echo "usage: bash $0 <image1.ext4 path|-l> <kernel path> <init path> [ramdisk path]" >&2
		exit 2
	fi

	IMAGE1="$1"
	KERNEL="$2"
	INIT="$3"
	RAMDISK="${4:-}"

	mkdir -p qemu-logs

	if [ "$IMAGE1" = "-l" ]; then
		if [ -z "$RAMDISK" ]; then
			echo "light mode (-l) requires a ramdisk path" >&2
			exit 2
		fi
		echo "=== booting kernel: $KERNEL (LIGHT MODE: no image1.ext4, ramdisk=$RAMDISK, rdinit=$INIT) ==="
		set -- \
			-M virt -cpu max -m 2G -smp 2 -nographic -no-reboot \
			-kernel "$KERNEL" \
			-initrd "$RAMDISK" \
			-append "console=ttyAMA0 rdinit=$INIT earlycon panic=-1"
	elif [ -n "$RAMDISK" ]; then
		echo "=== booting kernel: $KERNEL (ramdisk=$RAMDISK, rdinit=$INIT) ==="
		set -- \
			-M virt -cpu max -m 2G -smp 2 -nographic -no-reboot \
			-kernel "$KERNEL" \
			-initrd "$RAMDISK" \
			-drive file="$IMAGE1",if=none,id=image1,format=raw \
			-device nvme,serial=11451401,drive=image1 \
			-append "console=ttyAMA0 rdinit=$INIT earlycon panic=-1"
	else
		echo "=== booting kernel: $KERNEL (no ramdisk, init=$INIT) ==="
		set -- \
			-M virt -cpu max -m 2G -smp 2 -nographic -no-reboot \
			-kernel "$KERNEL" \
			-drive file="$IMAGE1",if=none,id=image1,format=raw \
			-device nvme,serial=11451401,drive=image1 \
			-append "console=ttyAMA0 root=/dev/nvme0n1 init=$INIT earlycon panic=-1"
	fi

	timeout --signal=KILL 120 qemu-system-aarch64 "$@" 2>&1
	status=$?
	echo "QEMU exited with status $status"

	exit "0"
fi

if [ "$(/busybox basename "$0")" = "init" ]; then
	# ==================================================================
	# mode 3: stage 1 (initramfs, PID 1 as /init)
	# ==================================================================
	PATH=/
	busybox mkdir -p /sys /dev /newroot /proc
	busybox mount -t sysfs sysfs /sys
	busybox mount -t proc proc /proc
	busybox mdev -s
	busybox echo "=== LKM4CTR_QEMU_TEST: stage1 (initramfs) ==="

	if busybox test -e /dev/nvme0n1 && busybox mount -t ext4 /dev/nvme0n1 /newroot; then
		busybox echo "=== LKM4CTR_QEMU_TEST: nvme available, proceeding to stage 2 ==="

		busybox cp /lkm4ctr_checker /newroot/
		busybox cp /lkm4ctr.ko /newroot/
		busybox cat /init > /newroot/second_init
		busybox chmod 755 /newroot/second_init

		busybox echo "=== LKM4CTR_QEMU_TEST: exec second init ==="
		# switch_root replaces PID 1 with the given command, run under the new
		# root; /busybox (copied onto image1.ext4 above) provides "env" here
		# since image1.ext4's own /system/bin/env may not exist yet at this
		# point, but /system/bin/sh (the real root's bionic-linked shell,
		# already baked into image1.ext4) is used to interpret /second_init so
		# stage 2 runs under the actual target userland's shell, not busybox's.
		exec busybox switch_root /newroot /busybox env -i /system/bin/sh /second_init
	fi

	busybox echo "=== LKM4CTR_QEMU_TEST: no nvme device, running LIGHT MODE checker directly ==="
	busybox cat /init > /tester
	busybox chmod 755 /tester
	busybox sh /tester -t /lkm4ctr_checker /lkm4ctr.ko

	lkm4ctr_do_reboot light
fi

# ======================================================================
# mode 4: stage 2 (real root, image1.ext4, PID 1 as /second_init)
# ======================================================================
#
# All ramdisk utilities are invoked with absolute paths: bionic's dynamic
# linker needs /proc/self/exe (or a resolvable argv[0]) to load shared
# objects, which only holds once /proc is mounted and the path is absolute.
set -x
# /dev is not preserved across switch_root (it was never a separate mount in
# stage 1, just mdev-populated directory entries on the initramfs, which
# switch_root discards along with the rest of the old root), so it must be
# populated again here the same way: mount sysfs, then mdev -s. Re-point
# stdio at /dev/kmsg immediately afterwards -- PID 1's original fds are
# otherwise easy to lose track of once /dev is replaced, silently dropping
# every echo/trace/command-output line for the rest of the script even
# though it keeps executing. /dev/kmsg writes become kernel printk records,
# which reliably reaches the QEMU console log.

# /do-mounts.sh and /do-umounts.sh (sourced below) are baked into
# image1.ext4's own root (not part of this ramdisk), so they only exist
# once switch_root has actually landed here; they mount/unmount the
# standard set of pseudo-filesystems (proc, /dev/pts, cgroups, etc.) that
# image1.ext4's userland (dockerd/containerd/runc) expects at runtime.
source /do-mounts.sh
PATH=/:$PATH

busybox mdev -s
ifconfig lo up

echo "=== LKM4CTR_QEMU_TEST: /lkm4ctr.ko + /lkm4ctr_checker copied onto the new root by stage1 ==="
chmod 755 /lkm4ctr_checker

echo "=== LKM4CTR_QEMU_TEST: running shared checker mode ==="
cp /second_init /tester
chmod 755 /tester
/system/bin/sh /tester -t /lkm4ctr_checker /lkm4ctr.ko

echo "=== LKM4CTR_QEMU_TEST: starting dockerd (daemon) ==="
dockerd &
for i in $(seq 1 30); do
  [ -S /var/run/docker.sock ] && break
  sleep 1
done

echo "=== LKM4CTR_QEMU_TEST: docker run (test container sanity) ==="
docker run --privileged --rm --network host -i docker.io/arm64v8/alpine:latest ps -e
docker run --privileged --rm --network host -i docker.io/arm64v8/ubuntu:latest ps -e

echo "=== LKM4CTR_QEMU_TEST: DONE ==="
# unmounts whatever /do-mounts.sh mounted above, so the following
# remount,ro is clean.
source /do-umounts.sh

lkm4ctr_do_reboot heavy
