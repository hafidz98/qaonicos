#!/bin/sh
# run-qemu.sh - Boot kernel Mach 3 QaonicOS di QEMU.
# Target dev: -M virt -cpu cortex-a7 -m 64 (RV1103 hardware asli = Fase D).
#
# Build dulu: cd kernel && ./build-md.sh
# Lalu: ./scripts/run-qemu.sh   (hentikan dengan Ctrl-A X)
set -e
cd "$(dirname "$0")/.."

# Image untuk blk_selftest (disarankan; tanpa device, blk_selftest
# FAIL graceful dan boot tetap lanjut).
IMG="${MACH3_BLK_IMG:-$HOME/workspace/qaonic_os/disk-images/mach3-blk.img}"
if [ ! -f "$IMG" ]; then
    echo "[run] membuat blk image 16MB (sparse): $IMG"
    truncate -s 16M "$IMG"
fi

exec qemu-system-arm -M virt -cpu cortex-a7 -m 64 -nographic \
    -L "$HOME/workspace/toolchain/usr/share/qemu" \
    -kernel kernel/build/mach3.elf \
    -drive file="$IMG",if=none,id=hd0,format=raw \
    -device virtio-blk-device,drive=hd0
