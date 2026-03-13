#!/usr/bin/env bash
# qemutest.sh - Boot an Android GKI kernel in QEMU aarch64 for testing
#
# Usage: qemutest.sh -b boot.img -i init_boot.img [-t timeout] [-q qemu_extra_args]
#
# Extracts the kernel from a full Android boot.img and the ramdisk from an
# Android init_boot.img, then boots them in qemu-system-aarch64.
#
# Kernel compression handled automatically: raw Image, Image.gz (native QEMU
# support), and Image.lz4 (auto-decompressed via lz4 tool).
# Ramdisk compression handled: lz4 (passed directly; GKI kernels have
# CONFIG_RD_LZ4=y), gzip, or uncompressed cpio (re-gzipped).
#
# Dependencies: qemu-system-aarch64, python3
#   Ubuntu: sudo apt install qemu-system-arm lz4
#   macOS:  brew install qemu lz4

set -euo pipefail

TIMEOUT=60
BOOT_IMG=""
INIT_BOOT_IMG=""
EXTRA_ARGS=""

# ─── usage ───────────────────────────────────────────────────────────────────

usage() {
    cat <<EOF
Usage: $(basename "$0") -b boot.img -i init_boot.img [options]

Boot an Android GKI kernel in QEMU aarch64 for kernel diagnostics.

  -b FILE      Full Android boot.img (header v0-v4, contains kernel)
  -i FILE      Android init_boot.img (header v3/v4, contains ramdisk/initramfs)
  -t SECONDS   QEMU timeout in seconds before killing (default: 60)
  -q ARGS      Extra arguments passed verbatim to qemu-system-aarch64
  -h           Show this help

Example:
  $(basename "$0") \\
    -b android14-6.1.75-2024-01-boot.img \\
    -i init_boot_global_3.0.3.0.WNIMIXM_16.0

The kernel boots on qemu-system-aarch64 -machine virt -cpu cortex-a57.
Android init will panic once it cannot find vendor partitions; the full
kernel dmesg is still captured and printed before the panic.

Dependencies:
  Required: qemu-system-aarch64, python3
  Optional: lz4  (required if boot.img has an lz4-compressed kernel)
  Ubuntu: sudo apt install qemu-system-arm lz4
  macOS:  brew install qemu lz4
EOF
}

# ─── argument parsing ─────────────────────────────────────────────────────────

while getopts "b:i:t:q:h" opt; do
    case "$opt" in
        b) BOOT_IMG="$OPTARG" ;;
        i) INIT_BOOT_IMG="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        q) EXTRA_ARGS="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

[[ -n "$BOOT_IMG" ]]      || { echo "Error: -b boot.img is required"; echo; usage; exit 1; }
[[ -n "$INIT_BOOT_IMG" ]] || { echo "Error: -i init_boot.img is required"; echo; usage; exit 1; }
[[ -f "$BOOT_IMG" ]]      || { echo "Error: boot.img not found: $BOOT_IMG"; exit 1; }
[[ -f "$INIT_BOOT_IMG" ]] || { echo "Error: init_boot.img not found: $INIT_BOOT_IMG"; exit 1; }

# ─── dependency checks ────────────────────────────────────────────────────────

if ! command -v qemu-system-aarch64 &>/dev/null; then
    echo "Error: qemu-system-aarch64 not found."
    echo "  Ubuntu/Debian: sudo apt install qemu-system-arm"
    echo "  macOS:         brew install qemu"
    exit 1
fi

if ! command -v python3 &>/dev/null; then
    echo "Error: python3 is required for parsing Android boot image headers."
    exit 1
fi

# ─── work directory (auto-cleaned on exit) ────────────────────────────────────

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

# ─── Android boot image header parser ────────────────────────────────────────
#
# Outputs: header_version kernel_size kernel_offset ramdisk_size ramdisk_offset
#
# Header layout reference (page_size fixed at 4096 for v3/v4):
#   v0-v2: magic(8) kernel_size(4) kernel_addr(4) ramdisk_size(4) ... page_size@36
#   v3/v4: magic(8) kernel_size(4) ramdisk_size(4) ... header_version@40
#   kernel_offset  = page_size
#   ramdisk_offset = page_size + ceil(kernel_size / page_size) * page_size

_parse_android_header() {
    local image="$1"
    python3 - "$image" <<'PYEOF'
import struct, sys

path = sys.argv[1]
with open(path, 'rb') as fh:
    hdr = fh.read(4096)

magic = hdr[0:8]
if magic != b'ANDROID!':
    print(f"ERROR: {path}: not an Android boot image (magic={magic!r})", file=sys.stderr)
    sys.exit(1)

# header_version at offset 40 for all versions (v0 stores dt_size here, value <10)
header_version = struct.unpack_from('<I', hdr, 40)[0]

if header_version >= 3:
    kernel_size   = struct.unpack_from('<I', hdr,  8)[0]
    ramdisk_size  = struct.unpack_from('<I', hdr, 12)[0]
    page_size     = 4096
else:
    kernel_size   = struct.unpack_from('<I', hdr,  8)[0]
    ramdisk_size  = struct.unpack_from('<I', hdr, 16)[0]
    page_size     = struct.unpack_from('<I', hdr, 36)[0]

kernel_pages   = (kernel_size  + page_size - 1) // page_size if kernel_size  > 0 else 0
kernel_offset  = page_size
ramdisk_offset = page_size + kernel_pages * page_size

print(header_version, kernel_size, kernel_offset, ramdisk_size, ramdisk_offset)
PYEOF
}

# ─── detect compression of a raw byte stream ─────────────────────────────────

_detect_compression() {
    local file="$1"
    python3 - "$file" <<'PYEOF'
import sys
with open(sys.argv[1], 'rb') as fh:
    magic = fh.read(6)
if magic[:2]  == b'\x1f\x8b':              print('gzip')
elif magic[:4] == b'\x02\x21\x4c\x18':    print('lz4_legacy')
elif magic[:4] == b'\x04\x22\x4d\x18':    print('lz4_frame')
elif magic[:6] == b'\xfd7zXZ\x00':        print('xz')
elif magic[:2] == b'BZ':                  print('bzip2')
elif magic[:6] == b'070701':              print('cpio_newc')      # ASCII "070701"
elif magic[:6] == b'070702':              print('cpio_newc_crc')  # ASCII "070702"
else:                                      print('raw')
PYEOF
}

# ─── parse boot.img ──────────────────────────────────────────────────────────

echo "=== Parsing boot.img: $BOOT_IMG ==="
read -r boot_ver boot_ksize boot_koff boot_rsize boot_roff \
    < <(_parse_android_header "$BOOT_IMG")

printf "  Header version : v%s\n" "$boot_ver"
printf "  Kernel         : %d bytes at offset %d\n" "$boot_ksize" "$boot_koff"
printf "  Ramdisk        : %d bytes\n" "$boot_rsize"

if [[ "$boot_ksize" -eq 0 ]]; then
    echo "Error: boot.img has no kernel (kernel_size=0). Supply a full boot.img."
    exit 1
fi

# Extract kernel
dd if="$BOOT_IMG" of="$WORKDIR/kernel.raw" \
    bs=1 skip="$boot_koff" count="$boot_ksize" 2>/dev/null
printf "  Extracted kernel: %d bytes\n" "$(wc -c < "$WORKDIR/kernel.raw")"

# Detect kernel compression and prepare for QEMU
KCOMP=$(_detect_compression "$WORKDIR/kernel.raw")
case "$KCOMP" in
    gzip)
        echo "  Kernel format: gzip (Image.gz) — QEMU handles natively"
        KERNEL_ARG="$WORKDIR/kernel.raw"
        ;;
    lz4_legacy | lz4_frame)
        echo "  Kernel format: lz4 — decompressing for QEMU"
        if ! command -v lz4 &>/dev/null; then
            echo "Error: lz4 tool required to decompress lz4 kernel."
            echo "  Ubuntu/Debian: sudo apt install lz4"
            echo "  macOS:         brew install lz4"
            exit 1
        fi
        lz4 -d -f "$WORKDIR/kernel.raw" "$WORKDIR/kernel.img" 2>/dev/null
        KERNEL_ARG="$WORKDIR/kernel.img"
        printf "  Decompressed to: %d bytes\n" "$(wc -c < "$WORKDIR/kernel.img")"
        ;;
    *)
        echo "  Kernel format: uncompressed (Image)"
        KERNEL_ARG="$WORKDIR/kernel.raw"
        ;;
esac

# ─── parse init_boot.img ─────────────────────────────────────────────────────

echo ""
echo "=== Parsing init_boot.img: $INIT_BOOT_IMG ==="
read -r ib_ver ib_ksize ib_koff ib_rsize ib_roff \
    < <(_parse_android_header "$INIT_BOOT_IMG")

printf "  Header version : v%s\n" "$ib_ver"
printf "  Ramdisk        : %d bytes at offset %d\n" "$ib_rsize" "$ib_roff"

if [[ "$ib_rsize" -eq 0 ]]; then
    echo "Error: init_boot.img has no ramdisk (ramdisk_size=0)."
    exit 1
fi

# Extract ramdisk
dd if="$INIT_BOOT_IMG" of="$WORKDIR/ramdisk.raw" \
    bs=1 skip="$ib_roff" count="$ib_rsize" 2>/dev/null
printf "  Extracted ramdisk: %d bytes\n" "$(wc -c < "$WORKDIR/ramdisk.raw")"

# Detect ramdisk compression and prepare for QEMU
# Android GKI kernels have CONFIG_RD_LZ4=y so lz4 initrd can be passed directly.
# gzip is always supported. Raw cpio is re-gzipped.
RDCOMP=$(_detect_compression "$WORKDIR/ramdisk.raw")
case "$RDCOMP" in
    gzip)
        echo "  Ramdisk format: gzip — using directly"
        INITRD_ARG="$WORKDIR/ramdisk.raw"
        ;;
    lz4_legacy | lz4_frame)
        echo "  Ramdisk format: lz4 — GKI kernel supports RD_LZ4, using directly"
        INITRD_ARG="$WORKDIR/ramdisk.raw"
        ;;
    cpio_newc | cpio_newc_crc)
        echo "  Ramdisk format: uncompressed cpio — gzipping for QEMU"
        gzip -9 < "$WORKDIR/ramdisk.raw" > "$WORKDIR/ramdisk.cpio.gz"
        INITRD_ARG="$WORKDIR/ramdisk.cpio.gz"
        ;;
    *)
        echo "  Ramdisk format: unknown ($RDCOMP) — passing as-is"
        INITRD_ARG="$WORKDIR/ramdisk.raw"
        ;;
esac

# ─── boot in QEMU ────────────────────────────────────────────────────────────

echo ""
echo "=== Booting kernel in QEMU aarch64 ==="
printf "  Kernel   : %s\n" "$KERNEL_ARG"
printf "  Initrd   : %s\n" "$INITRD_ARG"
printf "  Timeout  : %ss\n" "$TIMEOUT"
echo ""
echo "--- Boot log ---"
echo ""

# QEMU with -nographic uses the controlling TTY directly when stdin is a TTY,
# which would bypass stdout redirection (e.g. '| tee log.txt'). Redirecting
# stdin from /dev/null prevents that: QEMU then writes to stdout as normal.
# Ctrl-A X to kill QEMU interactively is therefore not available; use -t to
# set a timeout or Ctrl-C to abort.

EC=0
# $EXTRA_ARGS is intentionally unquoted: it may contain multiple space-separated
# QEMU flags passed by the caller via -q, and needs word splitting to work.
# shellcheck disable=SC2086
timeout "$TIMEOUT" qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a57 \
    -nographic \
    -no-reboot \
    -smp 2 \
    -m 2048 \
    -kernel  "$KERNEL_ARG" \
    -initrd  "$INITRD_ARG" \
    -append  "console=ttyAMA0 earlycon=pl011,0x09000000 loglevel=8 panic=10 printk.devkmsg=on androidboot.selinux=permissive" \
    $EXTRA_ARGS \
    < /dev/null || EC=$?

echo ""
case $EC in
    0)   echo "=== QEMU exited normally ===" ;;
    124) echo "=== Timed out after ${TIMEOUT}s ===" ;;
    *)   echo "=== QEMU exited with code $EC ===" ;;
esac
