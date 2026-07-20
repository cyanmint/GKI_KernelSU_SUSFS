#!/system/bin/sh
# Stage-2 init: runs as PID 1 once busybox switch_root (first_stage_init.sh,
# in the injected ramdisk) has handed control to this real root
# (image1.ext4). This script itself lives on image2.ext4 (attached as a
# second NVMe device, /dev/nvme1n1 -- the kernels under test have no virtio
# support), which first_stage_init.sh already mounted at /mnt inside
# image1.ext4 before switch_root, so shadow_ctr.ko/shadow_ctr_checker are
# already reachable under /mnt without any further mounting here. All
# ramdisk utilities are invoked with absolute paths: bionic's dynamic linker
# needs /proc/self/exe (or a resolvable argv[0]) to load shared objects,
# which only holds once /proc is mounted and the path is absolute.
set -x
# Mount devtmpfs/proc/sysfs first and immediately re-point stdio at
# /dev/kmsg instead of /dev/console: PID 1's original fds are otherwise easy
# to lose track of once /dev is replaced, silently dropping every
# echo/trace/command-output line for the rest of the script even though it
# keeps executing. /dev/kmsg writes become kernel printk records, which
# reliably reaches the QEMU console log.
/system/bin/mount -t devtmpfs devtmpfs /dev 2>/dev/null
exec </dev/console >/dev/kmsg 2>&1
/system/bin/mount -t proc proc /proc
/system/bin/mount -t sysfs sysfs /sys
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

echo "=== SHADOW_CTR_QEMU_TEST: image2.ext4 (module + checker) already mounted at /mnt by stage1 ==="
/system/bin/chmod 755 /mnt/shadow_ctr_checker

echo "=== SHADOW_CTR_QEMU_TEST: shadow_ctr_checker (pre-insmod) ==="
/mnt/shadow_ctr_checker
echo "shadow_ctr_checker (pre-insmod) -> $?"

echo "=== SHADOW_CTR_QEMU_TEST: inserting merged module ==="
/system/bin/insmod /mnt/shadow_ctr.ko
echo "insmod shadow_ctr.ko -> $?"
/system/bin/dmesg

echo "=== SHADOW_CTR_QEMU_TEST: shadow_ctr_checker (post-insmod) ==="
/mnt/shadow_ctr_checker
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
# must match TEST_CONTAINER_IMAGE in build-shadow-ctr.yml.
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
