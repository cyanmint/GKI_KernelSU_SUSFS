#!/busybox sh

set -x

lkm4ctr_run_qemu_mode() {
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
}

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

lkm4ctr_init_1() {

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
	busybox sh /init -t /lkm4ctr_checker /lkm4ctr.ko

	lkm4ctr_do_reboot light
}

lkm4ctr_init_2() {
	# All ramdisk utilities are invoked with absolute paths: bionic's dynamic
	# linker needs /proc/self/exe (or a resolvable argv[0]) to load shared
	# objects, which only holds once /proc is mounted and the path is absolute.

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
	/system/bin/sh /second_init -t /lkm4ctr_checker /lkm4ctr.ko

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
}

lkm4ctr_init_tail(){
	
}

if [ "$$" != "1" ]; then
	# ==================================================================
	# mode 1/2: host-side QEMU launcher, or checker mode ("-t")
	# ==================================================================
	set -u
	if [ "${1:-}" = "-t" ]; then
		if [ "$#" -ne 3 ]; then
			echo "usage: $0 -t <checker path> <module path>" >&2
			exit 2
		else
			lkm4ctr_run_checker_mode "$2" "$3"
			exit 0
		fi
	else
		if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
			echo "usage: bash $0 <image1.ext4 path|-l> <kernel path> <init path> [ramdisk path]" >&2
			exit 2
		else
			lkm4ctr_run_qemu_mode "$@"
			exit 0
		fi
	fi
else
	# ==================================================================
	# mode 3/4: init mode
	# ==================================================================
	case "$(/busybox basename "$0")" in
		init)
			lkm4ctr_init_1
			;;
		*)
			lkm4ctr_init_2
			;;
	esac
	lkm4ctr_init_tail
fi
