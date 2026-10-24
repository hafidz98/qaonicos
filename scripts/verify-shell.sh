#!/bin/sh
# verify-shell.sh - Verifikasi CLI qaon> via interaksi serial QEMU.
#
# Bukan cuma test code: boot beneran, kirim perintah via serial,
# verifikasi respons. Bukti visual = transkrip interaksi.
#
# Usage: ./scripts/verify-shell.sh
# Output: /tmp/shell-verify.log (transkrip) + PASS/FAIL di stdout.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TOOLCHAIN_QEMU_DIR="${TOOLCHAIN_QEMU_DIR:-$HOME/workspace/toolchain/usr/share/qemu}"
SOCK=/tmp/qaon-serial.sock
LOG=/tmp/shell-verify.log

rm -f "$SOCK" "$LOG"

# Always kill our QEMU, even when the python check FAILs under set -e.
# Without this, a FAIL exits before the kill lines below and the orphan
# QEMU holds the disk-image lock, poisoning the next script's boot.
cleanup() {
    if [ -n "${QPID:-}" ]; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
    fi
    pkill -f "qaon-serial[.]sock" 2>/dev/null || true
}
trap cleanup EXIT

# Boot QEMU: serial via unix socket (interaktif)
source "$HOME/workspace/toolchain/env.sh" 2>/dev/null || true
qemu-system-arm -M virt -cpu cortex-a7 -m 64 -nographic \
    -L "$TOOLCHAIN_QEMU_DIR" \
    -kernel "$ROOT/mach3-port/build/mach3.elf" \
    -drive file="$ROOT/disk-images/mach3-blk.img",if=none,id=hd0,format=raw \
    -device virtio-blk-device,drive=hd0 \
    -drive file="$ROOT/disk-images/sd128.img",if=none,id=hd1,format=raw \
    -device virtio-blk-device,drive=hd1 \
    -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
    -serial unix:"$SOCK",server,nowait \
    > /dev/null 2>&1 &
QPID=$!

# Tunggu socket
for i in $(seq 1 50); do
    [ -S "$SOCK" ] && break
    sleep 0.2
done
# Socket milik root (QEMU); buka permission agar user bisa connect
chmod 777 "$SOCK" 2>/dev/null || sudo chmod 777 "$SOCK" 2>/dev/null || true

# Interaksi via python: tunggu prompt, kirim perintah, catat respons
python3 - "$SOCK" "$LOG" <<'PYEOF'
import socket, sys, time

sock_path, log_path = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(50):
    try:
        s.connect(sock_path)
        break
    except OSError:
        time.sleep(0.2)

s.settimeout(2.0)
log = open(log_path, "w")
buf = b""

def read_until(prompt, timeout=90):
    global buf
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            d = s.recv(4096)
            if not d:
                time.sleep(0.1)
                continue
            buf += d
            log.write(d.decode("utf-8", "replace"))
            log.flush()
        except socket.timeout:
            pass
        if prompt in buf:
            out = buf
            buf = b""
            return out
    return None

transcript = b""

def cmd(c, expect):
    global buf, transcript
    start = len(transcript)
    s.sendall((c + "\n").encode())
    out = read_until(b"qaon> ", timeout=15)
    if out is None:
        print(f"TIMEOUT waiting for '{c}'")
        return False
    transcript += out
    # Drain late chunks, then search the whole span of this command.
    time.sleep(1.0)
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            transcript += d
            log.write(d.decode("utf-8", "replace"))
            log.flush()
            if b"qaon> " in d:
                break
    except (socket.timeout, OSError):
        pass
    ok = expect.encode() in transcript[start:]
    print(f"{'PASS' if ok else 'FAIL'}: '{c}' -> {'found' if ok else 'MISSING'} '{expect}'")
    return ok

# Tunggu prompt pertama (boot ~60-90 dtk)
print("waiting for qaon> prompt...", flush=True)
if read_until(b"qaon> ") is None:
    print("FAIL: boot timeout, no qaon> prompt")
    sys.exit(1)
print("got prompt, testing commands...", flush=True)

results = []
results.append(cmd("help", "list commands"))
results.append(cmd("uptime", "uptime"))
results.append(cmd("time", "20"))
results.append(cmd("ls", "/sd:"))
results.append(cmd("cat /motd.txt", "QaonicOS"))
results.append(cmd("qfetch", "QaonicOS (Mach 3.0)"))
# FND-4 hygiene: this used to expect "hello.txt" in the
# listing, but that file was the B1 self-test's canary
# leaking into /data (the self-test formatted the same RAM
# disk /data mounted). The self-test now runs on its own
# RAM disk; seed a real file first, then list for it.
results.append(cmd("put /data/note.txt hello qaonic", "put: ok"))
results.append(cmd("ls /data", "note.txt"))
results.append(cmd("cat /data/note.txt", "hello qaonic"))

s.close()
log.close()
if all(results):
    print("SHELL-VERIFY: PASS")
else:
    print("SHELL-VERIFY: FAIL")
    sys.exit(1)
PYEOF
RC=$?
# QEMU cleanup is the EXIT trap above, not here: under set -e a failing
# python check exits before this point, so kill lines here never run.
echo "--- transkrip: $LOG ---"
exit $RC
