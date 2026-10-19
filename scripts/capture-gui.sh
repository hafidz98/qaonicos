#!/bin/sh
# capture-gui.sh - Capture tampilan GUI QaonicOS (face/display) di QEMU.
#
# Verifikasi visual: bukan cuma test code PASS, tapi bukti display
# beneran ke-render. Output: PNG screenshot + info resolusi.
#
# Usage: ./scripts/capture-gui.sh [output.png]
# Note: butuh virtio-gpu; di QEMU headless pakai screendump via monitor.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TOOLCHAIN_QEMU_DIR="${TOOLCHAIN_QEMU_DIR:-$HOME/workspace/toolchain/usr/share/qemu}"
OUT="${1:-/tmp/qaonicos-gui.png}"
MON=/tmp/qaon-gui-mon
PPM=/tmp/qaon-gui.ppm

rm -f "$MON" "$PPM"
source "$HOME/workspace/toolchain/env.sh" 2>/dev/null || true

qemu-system-arm -M virt -cpu cortex-a7 -m 64 \
    -L "$TOOLCHAIN_QEMU_DIR" \
    -kernel "$ROOT/kernel/build/mach3.elf" \
    -drive file="$ROOT/disk-images/mach3-blk.img",if=none,id=hd0,format=raw \
    -device virtio-blk-device,drive=hd0 \
    -drive file="$ROOT/disk-images/sd128.img",if=none,id=hd1,format=raw \
    -device virtio-blk-device,drive=hd1 \
    -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
    -device virtio-gpu-device \
    -display none \
    -serial file:/tmp/qaon-gui-serial.log \
    -monitor unix:"$MON",server,nowait \
    > /dev/null 2>&1 &
QPID=$!

# Tunggu monitor + boot sampai face jalan (~90 dtk)
for i in $(seq 1 50); do
    [ -S "$MON" ] && break
    sleep 0.2
done
chmod 777 "$MON" 2>/dev/null || true
echo "[capture] menunggu face..."
for i in $(seq 1 55); do
    grep -q "face: display.*ok" /tmp/qaon-gui-serial.log 2>/dev/null && break
    sleep 2
done
sleep 3

# Screendump via monitor
printf 'screendump %s\n' "$PPM" | socat - UNIX-CONNECT:"$MON" > /dev/null 2>&1
sleep 2
chmod 644 "$PPM" 2>/dev/null || sudo chmod 644 "$PPM" 2>/dev/null || true

kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true

if [ -f "$PPM" ]; then
    python3 -c "
from PIL import Image
img = Image.open('$PPM')
print(f'[capture] display: {img.size[0]}x{img.size[1]} {img.mode}')
img.save('$OUT')
print(f'[capture] tersimpan: $OUT')
"
else
    echo "[capture] GAGAL: tidak ada output screendump"
    exit 1
fi
