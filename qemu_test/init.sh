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
#   3. copy /ctr (shadow_ctr.ko + shadow_ctr_checker, baked into this same
#      ramdisk) onto the new root.
#   4. copy this very script onto the new root as /second_init (`cat /init`
#      works because /init is this script's own path in the initramfs).
#   5. copy /busybox onto the new root, in case stage 2 needs it (e.g. to
#      populate /dev again with mdev, since switch_root does not preserve
#      the initramfs's un-mounted /dev contents).
#   6. busybox switch_root into /newroot and exec /second_init -- since its
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
	/busybox mkdir -p /sys
	/busybox mount -t sysfs sysfs /sys
	/busybox mdev -s
	exec </dev/console >/dev/kmsg 2>&1
	/busybox echo "=== SHADOW_CTR_QEMU_TEST: stage1 (initramfs) ==="

	/busybox mount -t ext4 /dev/nvme0n1 /newroot
	/busybox echo "mount image1.ext4 -> $?"

	/busybox cp -R /ctr /newroot/
	/busybox cat /init > /newroot/second_init
	/busybox chmod 755 /newroot/second_init
	/busybox cp /busybox /newroot/

	exec /busybox switch_root /newroot /second_init
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
/busybox mount -t sysfs sysfs /sys 2>/dev/null
/busybox mdev -s
exec </dev/console >/dev/kmsg 2>&1
/system/bin/mount -t proc proc /proc
export PATH=/system/bin
/system/bin/mkdir -p /dev/pts
/system/bin/mount -t devpts devpts /dev/pts
/system/bin/mkdir -p /sys/fs/cgroup
/system/bin/mount -t cgroup2 cgroup2 /sys/fs/cgroup || /system/bin/mount -t tmpfs cgroup /sys/fs/cgroup
# runc's pivot_root() refuses to operate with the initramfs-derived root as
# its old root ("invalid argument"); bind-mounting / onto itself turns it
# into an ordinary mount entry that pivot_root can pivot away from.
/system/bin/mount -o bind / /
# A proper tmpfs (with xattr support) at /tmp: shadow_ctr_checker's overlay2
# self-test mkdtemp()s its lower/upper/work dirs under /tmp, and it also
# backs dockerd's own vfs storage driver.
/system/bin/mkdir -p /tmp
/system/bin/mount -t tmpfs tmpfs /tmp
/system/bin/ifconfig lo up

echo "=== SHADOW_CTR_QEMU_TEST: /ctr (module + checker) copied onto the new root by stage1 ==="
/system/bin/chmod 755 /ctr/shadow_ctr_checker

echo "=== SHADOW_CTR_QEMU_TEST: shadow_ctr_checker (pre-insmod) ==="
/ctr/shadow_ctr_checker
echo "shadow_ctr_checker (pre-insmod) -> $?"

echo "=== SHADOW_CTR_QEMU_TEST: inserting merged module ==="
/system/bin/insmod /ctr/shadow_ctr.ko
echo "insmod shadow_ctr.ko -> $?"
/system/bin/dmesg

echo "=== SHADOW_CTR_QEMU_TEST: shadow_ctr_checker (post-insmod) ==="
/ctr/shadow_ctr_checker
echo "shadow_ctr_checker (post-insmod) -> $?"

echo "=== SHADOW_CTR_QEMU_TEST: configuring dockerd (vfs storage) ==="
/system/bin/mkdir -p /etc/docker
cat > /etc/docker/daemon.json <<'JSON'
{
  "storage-driver": "vfs",
  "iptables": false,
  "bridge": "none"
}
JSON

echo "=== SHADOW_CTR_QEMU_TEST: starting dockerd (daemon) ==="
dockerd --config-file=/etc/docker/daemon.json > /dockerd.log 2>&1 &
for i in $(/system/bin/seq 1 30); do
  [ -S /var/run/docker.sock ] && break
  /system/bin/sleep 1
done

# /alpine.tar (already sitting at the root of this filesystem, i.e.
# image1.ext4) is a `docker export` of a container's flat filesystem, not a
# `docker save` image archive, so it must be reconstituted with `docker
# import` (which tags a flat rootfs tarball as an image), not `docker load`
# (which only understands docker save's manifest+layers format). This tag
# must match the image image1.ext4 was baked with (docker.io/arm64v8/alpine:latest).
echo "=== SHADOW_CTR_QEMU_TEST: docker import (alpine tarball) ==="
docker import /alpine.tar docker.io/arm64v8/alpine:latest
echo "docker import -> $?"

echo "=== SHADOW_CTR_QEMU_TEST: docker run (test container sanity) ==="
docker run --privileged --rm --network host -it docker.io/arm64v8/alpine:latest true < /dev/null
echo "docker run -> $?"

echo "=== SHADOW_CTR_QEMU_TEST: dockerd log ==="
cat /dockerd.log

echo "=== SHADOW_CTR_QEMU_TEST: DONE ==="
echo o > /proc/sysrq-trigger
while true; do :; done
