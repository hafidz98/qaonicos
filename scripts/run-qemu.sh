#!/bin/sh
# run-qemu.sh - Boot kernel Mach 3 QaonicOS di QEMU.
# Target dev: -M virt -cpu cortex-a7 -m 64 (RV1103 hardware asli = Fase D).
#
# Build dulu: cd kernel && ./build-md.sh
# Lalu: ./scripts/run-qemu.sh   (hentikan dengan Ctrl-A X)
#
# Fase D: dua virtio-blk (hd0 = storage internal, hd1 = kartu SD 128MB
# berformat FAT32 fresh tiap boot) + virtio-net (user-mode NAT,
# hostfwd TCP 18080 -> guest 80 untuk server HTTP).
# App A1: + virtio-gpu-device (display 240x240 Qabot).
# QAON_VNC=:99 -> tambah server VNC di display :99 (capture verifikasi).
set -e
cd "$(dirname "$0")/.."

# Image untuk blk_selftest (disarankan; tanpa device, blk_selftest
# FAIL graceful dan boot tetap lanjut).
IMG="${MACH3_BLK_IMG:-$HOME/workspace/qaonic_os/disk-images/mach3-blk.img}"
if [ ! -f "$IMG" ]; then
    echo "[run] membuat blk image 16MB (sparse): $IMG"
    truncate -s 16M "$IMG"
fi

# Fase D: kartu SD 128MB dengan filesystem FAT32 fresh tiap boot
# (deterministik untuk uji usd/ufs; mkfat32.py murni Python).
SDIMG="$HOME/workspace/qaonic_os/disk-images/sd128.img"
echo "[run] format ulang FAT32: $SDIMG"
python3 tools/mkfat32.py create "$SDIMG" 128 >/dev/null

exec qemu-system-arm -M virt -cpu cortex-a7 -m 64 -nographic \
    -L "$HOME/workspace/toolchain/usr/share/qemu" \
    -kernel kernel/build/mach3.elf \
    -drive file="$IMG",if=none,id=hd0,format=raw \
    -device virtio-blk-device,drive=hd0 \
    -drive file="$SDIMG",if=none,id=hd1,format=raw \
    -device virtio-blk-device,drive=hd1 \
    -netdev user,id=net0,hostfwd=tcp::18080-:80 \
    -device virtio-net-device,netdev=net0 \
    -device virtio-gpu-device \
    ${QAON_VNC:+-vnc "$QAON_VNC"} "$@"
