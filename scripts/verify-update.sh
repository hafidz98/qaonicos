#!/bin/sh
# verify-update.sh - B4 A/B update manager verification (5 boots).
#
# Boot 1: update shows v1/slot A/idle; `update stage 2`.
# Boot 2: TRIAL boot; one-shots pass -> update CONFIRMED;
#   `update` shows v2/slot B/idle.
# Boot 3: `update stage 3` + `update sabotage` (arm bad boot).
# Boot 4: TRIAL boot for v3, healthy mark sabotaged -> the
#   trial stays unconfirmed.
# Boot 5: bootstate_init sees the unconfirmed trial -> ROLLBACK;
#   `update` shows v2/slot B/idle again; bootlog has ev=7.
#
# Output: UPDATE-VERIFY: PASS/FAIL.
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TOOLCHAIN_QEMU_DIR="${TOOLCHAIN_QEMU_DIR:-$HOME/workspace/toolchain/usr/share/qemu}"

source "$HOME/workspace/toolchain/env.sh" 2>/dev/null || true

python3 - "$ROOT" "$TOOLCHAIN_QEMU_DIR" <<'PYEOF'
import socket, subprocess, sys, time, os

root, qemu_dir = sys.argv[1], sys.argv[2]
SOCK = "/tmp/qaon-upd.sock"

def boot():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    p = subprocess.Popen([
        "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
        "-nographic", "-L", qemu_dir,
        "-kernel", root + "/kernel/build/mach3.elf",
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

def run_boot(tag):
    p, s = boot()
    sess = Sess(s)
    if sess.read_until(b"qaon> ") is None:
        print(f"FAIL: {tag} no prompt"); sys.exit(1)
    return p, s, sess

def end_boot(p, s):
    s.close(); p.kill(); p.wait()

# --- Boot 1: baseline + stage the next version ---
# (State-agnostic: the dev image keeps its version across runs.)
import re
p, s, sess = run_boot("boot1")
out = sess.cmd("update")
txt = (out or b"").decode("utf-8", "replace")
print("boot1:", txt.strip().splitlines()[-2:])
m = re.search(r"version=(\d+) slot=([AB])", txt)
if not m:
    print("FAIL: boot1 cannot parse update status"); sys.exit(1)
V0, SLOT0 = int(m.group(1)), m.group(2)
V1 = V0 + 1
V2 = V0 + 2
SLOT1 = "B" if SLOT0 == "A" else "A"
out = sess.cmd(f"update stage {V1}")
if b"staged" not in (out or b""):
    fails.append(f"boot1: stage {V1} failed")
end_boot(p, s)

# --- Boot 2: trial + confirm of v2 ---
p, s, sess = run_boot("boot2")
if b"update TRIAL" not in sess.all:
    fails.append("boot2: TRIAL banner missing")
if b"update CONFIRMED" not in sess.all:
    fails.append("boot2: CONFIRMED banner missing")
out = sess.cmd("update")
print("boot2:", (out or b"").decode("utf-8", "replace").strip().splitlines()[-2:])
if f"version={V1}".encode() not in (out or b"") or f"slot={SLOT1}".encode() not in (out or b""):
    fails.append(f"boot2: expected version={V1} slot={SLOT1} after confirm")
end_boot(p, s)

# --- Boot 3: stage v3 + sabotage ---
p, s, sess = run_boot("boot3")
out = sess.cmd(f"update stage {V2}")
if b"staged" not in (out or b""):
    fails.append(f"boot3: stage {V2} failed")
out = sess.cmd("update sabotage")
if b"sabotage armed" not in (out or b""):
    fails.append("boot3: sabotage arm failed")
end_boot(p, s)

# --- Boot 4: trial of v3, sabotaged (unconfirmed) ---
p, s, sess = run_boot("boot4")
if b"update TRIAL" not in sess.all:
    fails.append("boot4: TRIAL banner missing")
if b"sabotage armed" not in sess.all:
    fails.append("boot4: sabotage did not trigger")
if b"update CONFIRMED" in sess.all:
    fails.append("boot4: staged version must NOT be confirmed")
out = sess.cmd("update")
print("boot4:", (out or b"").decode("utf-8", "replace").strip().splitlines()[-2:])
if b"state=trial" not in (out or b""):
    fails.append("boot4: expected state=trial (unconfirmed)")
end_boot(p, s)

# --- Boot 5: rollback to v2 ---
p, s, sess = run_boot("boot5")
if b"update ROLLBACK" not in sess.all:
    fails.append("boot5: ROLLBACK banner missing")
out = sess.cmd("update")
print("boot5:", (out or b"").decode("utf-8", "replace").strip().splitlines()[-2:])
if f"version={V1}".encode() not in (out or b"") or f"slot={SLOT1}".encode() not in (out or b""):
    fails.append(f"boot5: expected rollback to version={V1} slot={SLOT1}")
out = sess.cmd(f"update stage {V0}")
if b"rejected (downgrade)" not in (out or b""):
    fails.append(f"boot5: downgrade to v{V0} was not rejected (B5)")
out = sess.cmd("bootlog")
if b"ev=7" not in (out or b""):
    fails.append("boot5: bootlog lacks ROLLBACK event (ev=7)")
end_boot(p, s)

if fails:
    print("UPDATE-VERIFY: FAIL")
    for f in fails:
        print(" -", f)
    sys.exit(1)
print("UPDATE-VERIFY: PASS")
PYEOF
