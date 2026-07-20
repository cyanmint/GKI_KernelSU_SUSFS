#!/busybox sh
# Stage-1 init: runs as PID 1 out of the injected (busybox) initramfs
# assembled by build-shadow-ctr.yml's build-test-assets job. Its only job is
# to mount image1.ext4 (attached as an NVMe device, /dev/nvme0n1 -- the
# kernels under test have no virtio support) and busybox switch_root's into
# it, handing control to second_stage_init.sh living at the root of
# image1.ext4.
/busybox mount -t devtmpfs devtmpfs /dev
exec </dev/console >/dev/kmsg 2>&1
/busybox echo "=== SHADOW_CTR_QEMU_TEST: stage1 (initramfs) ==="
/busybox mount -t ext4 /dev/nvme0n1 /newroot
/busybox echo "mount image1.ext4 -> $?"
exec /busybox switch_root /newroot /second_stage_init.sh
