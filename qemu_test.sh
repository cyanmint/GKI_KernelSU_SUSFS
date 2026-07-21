#!/busybox sh
# Merged host-launcher + stage-1/stage-2 init script. This single file plays
# three different roles, dispatched purely by PID and argv[0]/$0 -- no
# separate files/arguments select the mode:
#
#   1. Host-side QEMU launcher ("qemu_test.sh" mode), when $$ (the running
#      shell's own PID) is not 1 -- i.e. whenever this script is not acting
#      as some kernel's PID 1. Boots a single kernel under QEMU against a
#      given image1.ext4 (and, optionally, an injected ramdisk), for use in
#      both build-lkm4ctr.yml and ad-hoc manual debugging:
#
#        bash qemu_test.sh <image1.ext4 path> <kernel path> <init path> [ramdisk path]
#
#      If <ramdisk path> is given, boots with "-initrd <ramdisk path>" and
#      "rdinit=<init path>" (the injected ramdisk owns /, so the kernel is
#      told which program on *that* ramdisk to run as init). If omitted,
#      boots without "-initrd" at all, and "root=/dev/nvme0n1
#      init=<init path>" instead (image1.ext4 -- /dev/nvme0n1 -- is used
#      directly as the root filesystem, so <init path> must exist there).
#
#      IMPORTANT: this file's shebang is "#!/busybox sh" (needed for roles 2
#      and 3 below, where the kernel/switch_root exec it directly and only
#      /busybox is guaranteed to exist). That shebang is almost certainly
#      *not* usable on a plain Ubuntu host (no /busybox binary), so this
#      script must always be invoked explicitly as `bash qemu_test.sh ...`
#      on the host -- never `./qemu_test.sh ...`, which would honor the
#      shebang and try (and typically fail) to exec /busybox.
#
#   2. Stage-1 init (PID 1, argv[0]/$0 == "init"): this is the role played
#      when the script is baked into the injected (busybox) ramdisk as
#      /init and the kernel starts it as rdinit=/init. It:
#        a. populates /dev via mdev -- the QEMU test kernels have no
#           devtmpfs support, so /dev must be populated the old way: mount
#           sysfs, then `mdev -s` coldplugs every device node (console,
#           kmsg, nvme...) from /sys.
#        b. mounts image1.ext4 (/dev/nvme0n1, the prebuilt testsuite root
#           filesystem downloaded verbatim in build-lkm4ctr.yml) at
#           /newroot.
#        c. copies /lkm4ctr.ko + /lkm4ctr_checker, baked into this
#           same ramdisk, onto the new root.
#        d. copies this very script onto the new root as /second_init
#           (`cat /init` works because /init is this script's own path in
#           the initramfs).
#        e. busybox switch_root's into /newroot and execs /second_init --
#           since its argv[0] is now "/second_init" rather than "init", it
#           runs the stage-2 body below instead of stage-1 again.
#
#   3. Stage-2 init (PID 1, argv[0]/$0 != "init"): out of the real root,
#      image1.ext4, as /second_init (or, if booted directly against
#      image1.ext4 with no ramdisk at all via "init=/second_init" on the
#      kernel command line, straight away). Populates /dev again (same
#      reasoning as stage 1 -- switch_root does not preserve the
#      initramfs's un-mounted /dev contents), runs lkm4ctr_checker,
#      insmods lkm4ctr.ko, runs lkm4ctr_checker again, starts a real
#      dockerd, imports+runs the alpine tarball already baked into
#      image1.ext4, then powers off via sysrq.

if [ "$$" != "1" ]; then
	# ==================================================================
	# mode 1: host-side QEMU launcher ("qemu_test.sh")
	# ==================================================================
	set -uo pipefail
	# Note: intentionally not "set -e" -- QEMU's exit status (including a
	# timeout/KILL) is captured explicitly below so the console log is
	# always printed and this script's own exit status still reflects
	# QEMU's, rather than the shell aborting immediately on a nonzero
	# exit.

	if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
		echo "usage: bash $0 <image1.ext4 path> <kernel path> <init path> [ramdisk path]" >&2
		exit 2
	fi

	IMAGE1="$1"
	KERNEL="$2"
	INIT="$3"
	RAMDISK="${4:-}"

	mkdir -p qemu-logs

	if [ -n "$RAMDISK" ]; then
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

	exit "$status"
fi

if [ "$(/busybox basename "$0")" = "init" ]; then
	# ==================================================================
	# mode 2: stage 1 (initramfs, PID 1 as /init)
	# ==================================================================
	PATH=/
	busybox mkdir -p /sys /dev /newroot
	busybox mount -t sysfs sysfs /sys
	busybox mdev -s
	busybox echo "=== LKM4CTR_QEMU_TEST: stage1 (initramfs) ==="

	busybox mount -t ext4 /dev/nvme0n1 /newroot
	busybox echo "mount image1.ext4 -> $?"

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

# ======================================================================
# mode 3: stage 2 (real root, image1.ext4, PID 1 as /second_init)
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

echo "=== LKM4CTR_QEMU_TEST: /ctr (module + checker) copied onto the new root by stage1 ==="
chmod 755 /lkm4ctr_checker

echo "=== LKM4CTR_QEMU_TEST: lkm4ctr_checker (pre-insmod) ==="
lkm4ctr_checker

echo "=== LKM4CTR_QEMU_TEST: inserting merged module ==="
insmod /lkm4ctr.ko
dmesg

echo "=== LKM4CTR_QEMU_TEST: lkm4ctr_checker (post-insmod) ==="
lkm4ctr_checker

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
/system/bin/mount -o remount,ro /
echo o > /proc/sysrq-trigger
for i in 1 2 3 4 5; do
  echo $i
  sleep $i
done
exec env -i /system/bin/sh
