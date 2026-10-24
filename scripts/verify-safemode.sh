#!/bin/sh
# verify-safemode.sh - B2 multi-boot verification.
#
# Boot 1 (normal): bootctl shows fail_count=0 (healthy marked),
#   then `bootctl crash` arms a simulated crash.
# Boot 2: kernel must enter SAFE MODE (banner + daemons skipped),
#   shell still works; one-shots pass -> healthy resets counter.
# Boot 3: normal boot again (no safe mode).
#
# State persists in disk-images/mach3-blk.img between boots.
# Output: SAFEMODE-VERIFY: PASS/FAIL.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TOOLCHAIN_QEMU_DIR="${TOOLCHAIN_QEMU_DIR:-$HOME/workspace/toolchain/usr/share/qemu}"

source "$HOME/workspace/toolchain/env.sh" 2>/dev/null || true

python3 - "$ROOT" "$TOOLCHAIN_QEMU_DIR" <<'PYEOF'
import socket, subprocess, sys, time, os

root, qemu_dir = sys.argv[1], sys.argv[2]
SOCK = "/tmp/qaon-sm.sock"

def boot():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    p = subprocess.Popen([
        "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
        "-nographic", "-L", qemu_dir,
        "-kernel", root + "/mach3-port/build/mach3.elf",
        "-drive", f"file={root}/disk-images/mach3-blk.img,if=none,id=hd0,format=raw",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"file={root}/disk-images/sd128.img,if=none,id=hd1,format=raw",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=net0", "-device", "virtio-net-device,netdev=net0",
        "-serial", f"unix:{SOCK},server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        if os.path.exists(SOCK):
            break
        time.sleep(0.2)
    os.chmod(SOCK, 0o777)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(2.0)
    return p, s

class Sess:
    def __init__(self, s):
        self.s = s
        self.buf = b""
        self.all = b""
    def read_until(self, prompt, timeout=120):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                d = self.s.recv(4096)
                if d:
                    self.buf += d
                    self.all += d
            except socket.timeout:
                pass
            if prompt in self.buf:
                out, self.buf = self.buf, b""
                return out
        return None
    def cmd(self, c, timeout=15):
        self.s.sendall((c + "\n").encode())
        return self.read_until(b"qaon> ", timeout=timeout)

fails = []

# --- Boot 1: normal, then arm crash ---
p, s = boot()
sess = Sess(s)
if sess.read_until(b"qaon> ") is None:
    print("FAIL: boot 1 no prompt"); sys.exit(1)
out = sess.cmd("bootctl")
print("boot1 bootctl:", out.decode("utf-8", "replace").strip().splitlines()[-2:])
if b"fail_count=0" not in (out or b""):
    fails.append("boot1: fail_count != 0 after healthy boot")
out = sess.cmd("bootctl crash")
if b"crash armed" not in (out or b""):
    fails.append("boot1: crash arm failed")
sess.cmd("bootlog")
print("boot1 bootlog ok")
s.close(); p.kill(); p.wait()

# --- Boot 2: safe mode expected ---
p, s = boot()
sess = Sess(s)
if sess.read_until(b"qaon> ") is None:
    print("FAIL: boot 2 no prompt (safe mode shell broken)"); sys.exit(1)
if b"SAFE MODE" not in sess.all:
    fails.append("boot2: SAFE MODE banner missing")
if b"skipping daemon" not in sess.all:
    fails.append("boot2: daemons were not skipped")
out = sess.cmd("bootctl")
print("boot2 bootctl:", out.decode("utf-8", "replace").strip().splitlines()[-2:])
if b"safe_mode=1" not in (out or b""):
    fails.append("boot2: safe_mode != 1")
s.close(); p.kill(); p.wait()

# --- Boot 3: back to normal ---
p, s = boot()
sess = Sess(s)
if sess.read_until(b"qaon> ") is None:
    print("FAIL: boot 3 no prompt"); sys.exit(1)
if b"SAFE MODE" in sess.all:
    fails.append("boot3: still in safe mode after healthy boot")
out = sess.cmd("bootctl")
print("boot3 bootctl:", out.decode("utf-8", "replace").strip().splitlines()[-2:])
if b"fail_count=0" not in (out or b""):
    fails.append("boot3: fail_count != 0")
out = sess.cmd("bootlog")
log = out.decode("utf-8", "replace")
if "ev=3" not in log:
    fails.append("boot3: bootlog lacks SAFEMODE event record")
s.close(); p.kill(); p.wait()

if fails:
    print("SAFEMODE-VERIFY: FAIL")
    for f in fails:
        print(" -", f)
    sys.exit(1)
print("SAFEMODE-VERIFY: PASS")
PYEOF
