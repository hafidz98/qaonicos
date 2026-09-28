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

# Fase 15: kartu SD (emulasi) — image terpisah dari storage internal.
SD_IMG="${SD_IMG:-$HOME/workspace/mach-luckfox/sd128.img}"
if [ ! -f "$SD_IMG" ]; then
    echo "[run] membuat SD image 128MB (sparse): $SD_IMG"
    truncate -s 128M "$SD_IMG"
fi

# Fase 15: kartu SD (emulasi) — image terpisah dari storage internal.
# Identifikasi dev 0/dev 1 dilakukan kernel dari superblock
# ("QAONBLK1" vs "QAONSD01"); urutan -device hanya dipakai sebagai
# tiebreak di boot pertama (slot tertinggi = dev 0, sesuai observasi
# QEMU 8.2.2: -device terakhir -> slot MMIO terendah).
exec qemu-system-arm -M virt -cpu cortex-a7 -m 64 -nographic \
    -kernel kernel/mach-kernel.elf \
    -netdev user,id=net0,hostfwd=tcp::18080-:80 \
    -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 \
    -drive file="$IMG",if=none,id=hd0,format=raw \
    -device virtio-blk-device,drive=hd0 \
    -drive file="$SD_IMG",if=none,id=hd1,format=raw \
    -device virtio-blk-device,drive=hd1
