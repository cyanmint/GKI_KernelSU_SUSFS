#!/busybox sh
# Merged stage-1/stage-2 init: this single script is baked into the injected
# (busybox) ramdisk as /init and doubles as PID 1 of the real root
# (image1.ext4) once switch_root'd into it, as /second_init. It dispatches
# purely on argv[0]/$0 -- "init" runs the stage-1 body below, anything else
# (i.e. "second_init") runs the stage-2 body.
#
# Stage 1 (out of the injected busybox ramdisk, PID 1 as /init):
#   1. populate /dev via mdev -- the QEMU test kernels have no devtmpfs
#      support, so /dev must be populated the old way: mount sysfs, then
#      `mdev -s` coldplugs every device node (console, kmsg, nvme...) from
#      /sys.
#   2. mount image1.ext4 (/dev/nvme0n1, the prebuilt testsuite root
#      filesystem downloaded verbatim in build-shadow-ctr.yml) at /newroot.
#   3. copy /shadow_ctr.ko + /shadow_ctr_checker, baked into this same
#      ramdisk onto the new root.
#   4. copy this very script onto the new root as /second_init (`cat /init`
#      works because /init is this script's own path in the initramfs).
#   5. busybox switch_root into /newroot and exec /second_init -- since its
#      argv[0] is now "/second_init" rather than "init", it runs the
#      stage-2 body below instead of stage-1 again.
#
# Stage 2 (out of the real root, image1.ext4, PID 1 as /second_init):
#   populates /dev again (same reasoning as stage 1), runs
#   shadow_ctr_checker, insmods shadow_ctr.ko, runs shadow_ctr_checker
#   again, starts dockerd, imports+runs the alpine tarball already baked
#   into image1.ext4, then powers off via sysrq.

if [ "$(/busybox basename "$0")" = "init" ]; then
	# --- stage 1: initramfs ---
	PATH=/
	busybox mkdir -p /sys /dev /newroot
	busybox mount -t sysfs sysfs /sys
	busybox mdev -s
	busybox echo "=== SHADOW_CTR_QEMU_TEST: stage1 (initramfs) ==="

	busybox mount -t ext4 /dev/nvme0n1 /newroot
	busybox echo "mount image1.ext4 -> $?"

	busybox cp /shadow_ctr_checker /newroot/
	busybox cp /shadow_ctr.ko /newroot/
	busybox cat /init > /newroot/second_init
	busybox chmod 755 /newroot/second_init

    busybox echo "=== SHADOW_CTR_QEMU_TEST: exec second init ==="
	exec busybox switch_root /newroot /busybox env -i /system/bin/sh /second_init
fi

# --- stage 2: real root (image1.ext4) ---
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

source /do-mounts.sh
PATH=/:$PATH

busybox mdev -s
ifconfig lo up

echo "=== SHADOW_CTR_QEMU_TEST: /ctr (module + checker) copied onto the new root by stage1 ==="
chmod 755 /shadow_ctr_checker

echo "=== SHADOW_CTR_QEMU_TEST: shadow_ctr_checker (pre-insmod) ==="
shadow_ctr_checker

echo "=== SHADOW_CTR_QEMU_TEST: inserting merged module ==="
insmod /shadow_ctr.ko
dmesg

echo "=== SHADOW_CTR_QEMU_TEST: shadow_ctr_checker (post-insmod) ==="
shadow_ctr_checker

echo "=== SHADOW_CTR_QEMU_TEST: starting dockerd (daemon) ==="
dockerd &
for i in $(seq 1 30); do
  [ -S /var/run/docker.sock ] && break
  sleep 1
done

echo "=== SHADOW_CTR_QEMU_TEST: docker run (test container sanity) ==="
docker run --privileged --rm --network host -i docker.io/arm64v8/alpine:latest ps -e
docker run --privileged --rm --network host -i docker.io/arm64v8/ubuntu:latest ps -e

echo "=== SHADOW_CTR_QEMU_TEST: DONE ==="
source /do-umounts.sh
/system/bin/mount -o remount,ro /
echo o > /proc/sysrq-trigger
for i in 1 2 3 4 5; do
  echo $i
  sleep $i
done
exec env -i /system/bin/sh
