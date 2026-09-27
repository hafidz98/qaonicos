#!/bin/sh
# run-qemu.sh - Jalankan QaonicOS di QEMU dengan emulasi spek Luckfox Pico Mini.
#
#   -m 64            : RAM 64MB DDR2 (spek Pico Mini)
#   virtio-blk 128MB : storage (emulasi SPI NAND Flash 128MB)
#   virtio-net       : user-net, host meneruskan localhost:18080 -> guest:80
#
# Build dulu: ./kernel/build.sh
# Lalu: ./run-qemu.sh   (hentikan dengan Ctrl-A X)
set -e
cd "$(dirname "$0")"

IMG="${PICO_IMG:-$HOME/workspace/mach-luckfox/pico128.img}"
if [ ! -f "$IMG" ]; then
    echo "[run] membuat disk image 128MB (sparse): $IMG"
    truncate -s 128M "$IMG"
fi

exec qemu-system-arm -M virt -cpu cortex-a7 -m 64 -nographic \
    -kernel kernel/mach-kernel.elf \
    -netdev user,id=net0,hostfwd=tcp::18080-:80 \
    -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
    -drive file="$IMG",if=none,id=hd0,format=raw \
    -device virtio-blk-device,drive=hd0
