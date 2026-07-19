#!/system/bin/sh
/system/bin/mount -t proc proc /proc
/system/bin/mount -t sysfs sysfs /sys
/system/bin/mount -t devtmpfs devtmpfs /dev
M=/modules
/system/bin/insmod $M/shadow_hijack.ko
echo "insmod shadow_hijack.ko -> $?"
/system/bin/insmod $M/shadow_ns.ko
echo "insmod shadow_ns.ko -> $?"
for m in shadow_sysvipc shadow_mqueue shadow_cgdevices; do
  if [ -f "$M/$m.ko" ]; then
    /system/bin/insmod "$M/$m.ko"
    echo "insmod $m.ko -> $?"
  fi
done
echo "=== SHADOW_CTR_QEMU_TEST: dmesg ==="
/system/bin/dmesg
echo "=== SHADOW_CTR_QEMU_TEST: shadow_ctr_checker ==="
/shadow_ctr_checker
echo "shadow_ctr_checker -> $?"
echo "=== after checker dmesg ==="
/system/bin/dmesg | tail -30
echo "=== SHADOW_CTR_QEMU_TEST: DONE ==="
echo o > /proc/sysrq-trigger
while true; do :; done
