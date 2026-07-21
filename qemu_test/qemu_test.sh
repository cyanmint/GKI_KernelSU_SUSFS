#!/usr/bin/env bash
# Boot-test a single kernel under QEMU against the injected (busybox) ramdisk
# and the prebuilt image1.ext4 testsuite root filesystem.
#
# Extracted from .github/workflows/build-shadow-ctr.yml's "Boot all three
# kernels under QEMU" step so the same QEMU invocation can be reused/run
# locally without re-reading the workflow YAML.
#
# Usage:
#   qemu_test.sh <image1.ext4 path> <kernel path> <variant name>
#
# Expects "injected-ramdisk.cpio" (built by the "Build injected (busybox)
# ramdisk" workflow step) to exist in the current working directory, and
# writes its QEMU console log to "qemu-logs/qemu-console-<variant name>.log"
# (also relative to the current working directory).
#
# Exits with QEMU's own exit status (propagated from `timeout`).

set -u

if [ "$#" -ne 3 ]; then
	echo "usage: $0 <image1.ext4 path> <kernel path> <variant name>" >&2
	exit 2
fi

IMAGE1="$1"
KERNEL="$2"
VARIANT="$3"

mkdir -p qemu-logs
LOG="qemu-logs/qemu-console-$VARIANT.log"

echo "=== booting kernel variant: $VARIANT ==="
timeout --signal=KILL 300 \
	qemu-system-aarch64 \
		-M virt -cpu max -m 2G -smp 2 -nographic -no-reboot \
		-kernel "$KERNEL" \
		-initrd injected-ramdisk.cpio \
		-drive file="$IMAGE1",if=none,id=image1,format=raw \
		-device nvme,serial=11451401,drive=image1 \
		-append "console=ttyAMA0 rdinit=/init earlycon panic=-1" \
	> "$LOG" 2>&1
status=$?
echo "QEMU ($VARIANT) exited with status $status"
tail -n 100 "$LOG"

exit "$status"
