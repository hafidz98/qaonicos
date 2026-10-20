#!/bin/sh
# capture-gui.sh - Capture the QaonicOS GUI (face/dashboard) in QEMU.
#
# Boot UX dev flow: the OS boots to the shell; the dashboard is
# started with the `gui` shell command, driven here over the
# serial socket. Output: PNG screenshot + display info.
#
# Usage: ./scripts/capture-gui.sh [output.png]
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TOOLCHAIN_QEMU_DIR="${TOOLCHAIN_QEMU_DIR:-$HOME/workspace/toolchain/usr/share/qemu}"
OUT="${1:-/tmp/qaonicos-gui.png}"
MON=/tmp/qaon-gui-mon
SER=/tmp/qaon-gui-ser
PPM=/tmp/qaon-gui.ppm

rm -f "$MON" "$SER" "$PPM"
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
    -serial unix:"$SER",server,nowait \
    -monitor unix:"$MON",server,nowait \
    > /dev/null 2>&1 &
QPID=$!

python3 - "$SER" <<'EOF'
import os, socket, sys, time
path = sys.argv[1]
for _ in range(100):
    if os.path.exists(path):
        break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(path)
os.chmod(path, 0o777)
s.settimeout(0.5)
data = b""
t0 = time.time()
sent = False
while time.time() - t0 < 180:
    try:
        d = s.recv(4096)
        if d:
            data += d
    except socket.timeout:
        pass
    t = data.decode("utf-8", "replace")
    if not sent and "bootux: dev mode" in t:
        s.sendall(b"gui\n")
        sent = True
        print("[capture] gui command sent")
    if "face: Qabot server starting" in t:
        print("[capture] face started")
        time.sleep(4)
        break
s.close()
EOF

sleep 1
printf 'screendump %s\n' "$PPM" | socat - UNIX-CONNECT:"$MON" > /dev/null 2>&1
sleep 2
chmod 644 "$PPM" 2>/dev/null || true

kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true

if [ -f "$PPM" ]; then
    python3 -c "
from PIL import Image
img = Image.open('$PPM')
print(f'[capture] display: {img.size[0]}x{img.size[1]} {img.mode}')
img.save('$OUT')
print('[capture] saved: $OUT')
"
else
    echo "[capture] FAILED: no screendump" >&2
    exit 1
fi
