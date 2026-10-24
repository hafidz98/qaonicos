#!/bin/sh
# verify-firstboot.sh - B3 first-boot provisioning verification.
#
# Boot 1: device unprovisioned -> UFIRST banner printed; shell
#   `setup` shows NOT provisioned; then `setup name`/`setup done`.
# Boot 2: provisioned -> no banner; `setup` shows provisioned and
#   the saved name; then `setup reset`.
# Boot 3: unprovisioned again -> banner is back.
#
# Also checks UTIMEGATE: PASS in every boot log.
# Output: FIRSTBOOT-VERIFY: PASS/FAIL.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TOOLCHAIN_QEMU_DIR="${TOOLCHAIN_QEMU_DIR:-$HOME/workspace/toolchain/usr/share/qemu}"

source "$HOME/workspace/toolchain/env.sh" 2>/dev/null || true

python3 - "$ROOT" "$TOOLCHAIN_QEMU_DIR" <<'PYEOF'
import socket, subprocess, sys, time, os

root, qemu_dir = sys.argv[1], sys.argv[2]
SOCK = "/tmp/qaon-fb.sock"

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

def check_gate(sess, tag):
    if b"UTIMEGATE: PASS" not in sess.all:
        fails.append(f"{tag}: UTIMEGATE did not pass")

# --- Boot 1: first boot expected ---
p, s = boot()
sess = Sess(s)
if sess.read_until(b"qaon> ") is None:
    print("FAIL: boot 1 no prompt"); sys.exit(1)
check_gate(sess, "boot1")
if b"first boot setup" not in sess.all:
    fails.append("boot1: first-boot banner missing")
if b"UFIRST: first-boot" not in sess.all:
    fails.append("boot1: UFIRST did not report first-boot")
out = sess.cmd("setup")
if b"NOT provisioned" not in (out or b""):
    fails.append("boot1: setup status should be NOT provisioned")
out = sess.cmd("setup name myqaon")
if b"name saved" not in (out or b""):
    fails.append("boot1: setup name failed")
out = sess.cmd("setup")
if b"myqaon" not in (out or b""):
    fails.append("boot1: setup status does not show saved name")
out = sess.cmd("setup done")
if b"provisioned" not in (out or b""):
    fails.append("boot1: setup done failed")
s.close(); p.kill(); p.wait()

# --- Boot 2: provisioned ---
p, s = boot()
sess = Sess(s)
if sess.read_until(b"qaon> ") is None:
    print("FAIL: boot 2 no prompt"); sys.exit(1)
check_gate(sess, "boot2")
if b"first boot setup" in sess.all:
    fails.append("boot2: banner shown despite provisioned state")
if b"UFIRST: provisioned" not in sess.all:
    fails.append("boot2: UFIRST did not report provisioned")
out = sess.cmd("setup")
txt = out.decode("utf-8", "replace") if out else ""
if "NOT provisioned" in txt:
    fails.append("boot2: setup status should be provisioned")
# Name persistence across boots is NOT checked: in QEMU the
# littlefs backend is RAM (resets per boot). On hardware it is
# NAND-backed and persists (B1-HW). The provisioned flag lives
# on the blk state sector and does persist -- checked above.
out = sess.cmd("setup reset")
if b"cleared" not in (out or b""):
    fails.append("boot2: setup reset failed")
s.close(); p.kill(); p.wait()

# --- Boot 3: first boot again ---
p, s = boot()
sess = Sess(s)
if sess.read_until(b"qaon> ") is None:
    print("FAIL: boot 3 no prompt"); sys.exit(1)
check_gate(sess, "boot3")
if b"first boot setup" not in sess.all:
    fails.append("boot3: banner missing after reset")
s.close(); p.kill(); p.wait()

if fails:
    print("FIRSTBOOT-VERIFY: FAIL")
    for f in fails:
        print(" -", f)
    sys.exit(1)
print("FIRSTBOOT-VERIFY: PASS")
PYEOF
