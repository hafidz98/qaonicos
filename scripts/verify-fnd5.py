#!/usr/bin/env python3
"""FND5-VERIFY: update-package delivery path (FND-5).

`update stagepkg <path>` is the product delivery path for
signed updates: the shell reads a real QUP1 package FILE --
built on the host by scripts/sign-update.py with the dev
key -- from /sd (FAT) or /data (littlefs, persistent since
FND-4) and hands it to the existing kernel verify+stage
path (syscall 98 sub 9: bad signature/format -> -3,
downgrade -> -2). Pre-FND-5 the shell has no stagepkg, so
every stagepkg check below FAILs and nothing stages.

Placement is host-side, QEMU stopped:
  /sd   -- a fresh tools/mkfat32.py image plus a minimal
           FAT32 file-adder (this file): parse the BPB,
           allocate a cluster chain, write a root dirent,
           update both FAT copies.
  /data -- scripts/lfsimg.c, the vendored littlefs compiled
           for the host with the kernel port's exact
           lfs_config, writing into the reserved tail
           region of the blk image.

Flow:
  Hygiene: scrub the littlefs region to 0xFF, fresh SD.
  Probe boot: read current version/slot from `update`;
    `ls /data` makes the guest format/mount the fresh
    region; clean poweroff.
  Host: build packages (good V+1, valid-but-old V, tampered,
    truncated, oversized, empty); FAT-add all to /sd;
    lfsimg-put the good one to /data and get it back
    byte-exact.
  Boot 1: sub-7 regression (stage + cancel, unsigned route
    untouched); stagepkg negatives -- missing file on both
    filesystems, wrong root, empty, too large, tampered
    (-3), downgrade (-2), truncated (-3) -- with `update`
    proving state stays idle at V after each; then the
    happy path from /data (staged, pending_ver=V+1),
    cancel, and the happy path from /sd (staged for real).
  Boot 2: TRIAL banner, CONFIRMED banner, `update` shows
    V+1 on the flipped slot, idle.
  Boot 3: the very same package is now a downgrade (-2) --
    the delivery path end to end. Cleanup: /data file
    removed host-side, SD image recreated fresh; the disk
    is left healthy and idle at V+1 (battery scripts are
    version-agnostic by design, cf. verify-update.sh).

Harness rules (house): the serial socket is drained
continuously by a reader thread; leftover qemu-system-arm
is killed by exact name first. No GUI is launched: in dev
mode sh owns the console, so serial commands are safe.
"""
import os, re, socket, struct, subprocess, sys
import threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"

subprocess.run(["pkill", "-x", "qemu-system-arm"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)

IMG = os.path.join(ROOT, "disk-images", "mach3-blk.img")
SDIMG = os.path.join(ROOT, "disk-images", "sd128.img")
KEY = os.path.join(ROOT, "scripts", "update-img", "dev-key.pem")
TMP = "/tmp/fnd5"
os.makedirs(TMP, exist_ok=True)
LFSIMG = os.path.join(TMP, "lfsimg")

# littlefs region on the 16 MiB image (mirrors lfs_port.c).
TOTAL_SECTORS = 32768
TAIL_RESERVED = 18
REGION_SECTORS = 2048
REGION_BASE = TOTAL_SECTORS - TAIL_RESERVED - REGION_SECTORS

PKG_MAX = 8192

results = []


def check(name, cond):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name, flush=True)


# ----------------------------------------------------------------
# Host-side placement helpers
# ----------------------------------------------------------------

def scrub_lfs_region():
    with open(IMG, "r+b") as f:
        f.seek(REGION_BASE * 512)
        f.write(b"\xff" * (REGION_SECTORS * 512))


def fresh_sd():
    subprocess.run(["python3", os.path.join(ROOT, "tools",
                     "mkfat32.py"), "create", SDIMG, "128"],
                   check=True, stdout=subprocess.DEVNULL)


def fat_add(img_path, name, data):
    """Add one root-directory file to a FAT32 image."""
    with open(img_path, "rb") as f:
        img = bytearray(f.read())
    assert struct.unpack_from("<H", img, 11)[0] == 512
    spc = img[13]
    rsvd = struct.unpack_from("<H", img, 14)[0]
    nfats = img[16]
    fatsz = struct.unpack_from("<I", img, 36)[0]
    root_cl = struct.unpack_from("<I", img, 44)[0]
    fat_off = rsvd * 512
    data_start = rsvd + nfats * fatsz

    def clus_off(cl):
        return (data_start + (cl - 2) * spc) * 512

    def fat_get(i):
        return struct.unpack_from("<I", img, fat_off + i * 4)[0] \
            & 0x0FFFFFFF

    def fat_set(i, v):
        for k in range(nfats):
            struct.pack_into("<I", img,
                             (rsvd + k * fatsz) * 512 + i * 4, v)

    slot = None
    cl = root_cl
    while cl < 0x0FFFFFF8 and slot is None:
        base = clus_off(cl)
        for i in range(spc * 512 // 32):
            if img[base + i * 32] in (0x00, 0xE5):
                slot = base + i * 32
                break
        cl = fat_get(cl)
    assert slot is not None, "FAT root dir full"

    ncl = (len(data) + spc * 512 - 1) // (spc * 512) if data else 0
    chain = []
    nxt = 2
    while len(chain) < ncl:
        if fat_get(nxt) == 0:
            chain.append(nxt)
        nxt += 1
    for j, c in enumerate(chain):
        fat_set(c, chain[j + 1] if j + 1 < len(chain) else 0x0FFFFFFF)
        off = clus_off(c)
        chunk = data[j * spc * 512:(j + 1) * spc * 512]
        img[off:off + spc * 512] = chunk + \
            b"\x00" * (spc * 512 - len(chunk))

    stem, _, ext = name.partition(".")
    ent = bytearray(32)
    ent[0:8] = stem.encode().ljust(8, b" ")
    ent[8:11] = ext.encode().ljust(3, b" ")
    ent[11] = 0x20
    first = chain[0] if chain else 0
    struct.pack_into("<H", ent, 20, (first >> 16) & 0xFFFF)
    struct.pack_into("<H", ent, 26, first & 0xFFFF)
    struct.pack_into("<I", ent, 28, len(data))
    img[slot:slot + 32] = ent
    with open(img_path, "wb") as f:
        f.write(img)


def build_lfsimg():
    src = [os.path.join(ROOT, "scripts", "lfsimg.c"),
           os.path.join(ROOT, "mach3-port", "src", "ext",
                        "littlefs", "lfs.c"),
           os.path.join(ROOT, "mach3-port", "src", "ext",
                        "littlefs", "lfs_util.c")]
    subprocess.run(["cc", "-O2", "-I", os.path.join(
        ROOT, "mach3-port", "src", "ext", "littlefs"),
        "-o", LFSIMG] + src, check=True)


def lfsimg(*args):
    r = subprocess.run([LFSIMG, IMG] + list(args),
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"lfsimg {args}: {r.stderr.strip()}")
    return r.stdout


def sign(version, out):
    payload = os.path.join(TMP, "payload.bin")
    with open(payload, "wb") as f:
        f.write(b"QaonicOS FND-5 delivery-path test payload.\n" * 8)
    subprocess.run(["python3", os.path.join(ROOT, "scripts",
                     "sign-update.py"), "--key", KEY,
                    "--version", str(version), "--payload", payload,
                    "--out", out], check=True,
                   stdout=subprocess.DEVNULL)
    with open(out, "rb") as f:
        return f.read()


# ----------------------------------------------------------------
# QEMU boot harness (same shape as verify-fnd4.py)
# ----------------------------------------------------------------

class Boot:
    """One QEMU process + drained serial socket + cmd()."""

    def __init__(self, tag):
        self.tag = tag
        self.sock_path = f"/tmp/fnd5-{tag}.sock"
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass
        self.p = subprocess.Popen([
            "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7",
            "-m", "64", "-L", tc + "/usr/share/qemu",
            "-kernel", ROOT + "/mach3-port/build/mach3.elf",
            "-serial", f"unix:{self.sock_path},server,nowait",
            "-display", "none",
            "-drive", f"file={IMG},if=none,id=hd0,format=raw",
            "-device", "virtio-blk-device,drive=hd0",
            "-drive", f"file={SDIMG},if=none,id=hd1,format=raw",
            "-device", "virtio-blk-device,drive=hd1",
            "-netdev", "user,id=net0,hostfwd=tcp::18080-:80",
            "-device", "virtio-net-device,netdev=net0",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=env)
        self.buf = b""
        self.lock = threading.Lock()
        self.ser = None
        for _ in range(75):
            if os.path.exists(self.sock_path):
                try:
                    self.ser = socket.socket(socket.AF_UNIX,
                                             socket.SOCK_STREAM)
                    self.ser.connect(self.sock_path)
                    break
                except (ConnectionRefusedError, FileNotFoundError):
                    time.sleep(0.2)
            else:
                time.sleep(0.2)
        if self.ser is None:
            raise RuntimeError("no serial socket " + self.sock_path)
        self.ser.settimeout(0.3)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            try:
                d = self.ser.recv(4096)
                if d:
                    with self.lock:
                        self.buf += d
            except socket.timeout:
                pass
            except OSError:
                return

    def text(self):
        with self.lock:
            return self.buf

    def wait(self, needle, timeout=120):
        if isinstance(needle, str):
            needle = needle.encode()
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.text():
                return True
            time.sleep(0.2)
        return needle in self.text()

    def cmd(self, c, timeout=30):
        pos = len(self.text())
        self.ser.sendall(c.encode() + b"\n")
        t0 = time.time()
        while time.time() - t0 < timeout:
            seg = self.text()[pos:]
            if b"\n" in seg and seg.rstrip().endswith(b"qaon>"):
                return seg
            time.sleep(0.2)
        return self.text()[pos:]

    def wait_prompt(self, timeout=150):
        return self.wait(b"qaon> ", timeout)

    def poweroff(self, timeout=30):
        self.ser.sendall(b"poweroff\n")
        try:
            return self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            return None

    def kill(self):
        self.p.kill()
        self.p.wait()


def upd_status(b):
    out = b.cmd("update")
    m = re.search(rb"version=(\d+) slot=([AB]) state=(\w+)", out)
    if not m:
        return None
    pend = re.search(rb"pending_ver=(\d+)", out)
    return (int(m.group(1)), m.group(2).decode(),
            m.group(3).decode(),
            int(pend.group(1)) if pend else None)


def idle_at(b, v):
    st = upd_status(b)
    return st is not None and st[0] == v and st[2] == "idle"


# ================================================================
print("== hygiene: fresh images ==")
if not os.path.exists(IMG):
    os.makedirs(os.path.dirname(IMG), exist_ok=True)
    with open(IMG, "wb") as f:
        f.truncate(16 * 1024 * 1024)
scrub_lfs_region()
fresh_sd()
build_lfsimg()
check("hygiene: images prepared + lfsimg built", True)

# ================================================================
print("== probe boot: current version/slot ==")
b = Boot("p")
check("probe: shell prompt", b.wait_prompt())
st = upd_status(b)
check("probe: update status parses", st is not None)
V0, SLOT0 = st[0], st[1]
V1 = V0 + 1
SLOT1 = "B" if SLOT0 == "A" else "A"
print(f"  current version={V0} slot={SLOT0}; target {V1}")
out = b.cmd("ls /data")
check("probe: /data mounts (guest formats fresh region)",
      b"mount failed" not in out)
rc = b.poweroff()
check("probe: clean poweroff rc 0", rc == 0)

# ================================================================
print("== host: build + place packages ==")
good = sign(V1, os.path.join(TMP, "pkgnew.bin"))
old = sign(V0, os.path.join(TMP, "pkgold.bin"))
bad = bytearray(good)
bad[-1] ^= 0x01
cut = good[:200]
big = b"X" * (PKG_MAX + 808)
for name, data in [("PKGNEW.BIN", good), ("PKGOLD.BIN", old),
                   ("PKGBAD.BIN", bytes(bad)),
                   ("PKGCUT.BIN", cut), ("PKGBIG.BIN", big),
                   ("PKGZERO.BIN", b"")]:
    fat_add(SDIMG, name, data)
lfsimg("put", "pkgnew.bin", os.path.join(TMP, "pkgnew.bin"))
back = os.path.join(TMP, "pkgnew-back.bin")
lfsimg("get", "pkgnew.bin", back)
with open(back, "rb") as f:
    check("host: /data package round-trips byte-exact via lfsimg",
          f.read() == good)
out = lfsimg("ls")
check("host: /data lists pkgnew.bin with the package size",
      f"pkgnew.bin {len(good)}" in out)

# ================================================================
print("== boot 1: negatives first, then both happy paths ==")
b = Boot("1")
check("boot1: shell prompt", b.wait_prompt())

out = b.cmd(f"update stage {V0 + 5}")
check("boot1: legacy sub-7 stage still works",
      b"staged (reboot to trial)" in out)
st = upd_status(b)
check("boot1: sub-7 stage shows pending",
      st is not None and st[2] == "staged" and st[3] == V0 + 5)
out = b.cmd("update cancel")
check("boot1: cancel clears the sub-7 stage",
      b"cancelled" in out and idle_at(b, V0))

out = b.cmd("update stagepkg")
check("boot1: stagepkg without a path -> usage error",
      b"usage" in out and idle_at(b, V0))
out = b.cmd("update stagepkg /data/nothere.bin")
check("boot1: missing /data file -> clean error, nothing staged",
      b"cannot read" in out and idle_at(b, V0))
out = b.cmd("update stagepkg /sd/NOSUCH.BIN")
check("boot1: missing /sd file -> clean error, nothing staged",
      b"cannot read" in out and idle_at(b, V0))
out = b.cmd("update stagepkg /etc/pkg.bin")
check("boot1: unsupported root -> clean error, nothing staged",
      b"usage" in out and idle_at(b, V0))
out = b.cmd("update stagepkg /sd/PKGZERO.BIN")
check("boot1: empty file -> clean error, nothing staged",
      b"empty" in out and idle_at(b, V0))
out = b.cmd("update stagepkg /sd/PKGBIG.BIN")
check("boot1: oversized file -> too-large error, nothing staged",
      b"too large" in out and idle_at(b, V0))
out = b.cmd("update stagepkg /sd/PKGBAD.BIN")
check("boot1: tampered signature -> rejected -3, nothing staged",
      b"bad signature" in out and idle_at(b, V0))
out = b.cmd("update stagepkg /sd/PKGOLD.BIN")
check("boot1: valid signature, old version -> rejected -2",
      b"downgrade" in out and idle_at(b, V0))
out = b.cmd("update stagepkg /sd/PKGCUT.BIN")
check("boot1: truncated package -> rejected -3, nothing staged",
      b"bad signature" in out and idle_at(b, V0))

out = b.cmd("update stagepkg /data/pkgnew.bin")
check("boot1: stagepkg from /data stages the signed package",
      b"stagepkg:" in out and b"staged (reboot to trial)" in out)
st = upd_status(b)
check("boot1: staged state shows pending_ver=V+1",
      st is not None and st[2] == "staged" and st[3] == V1)
out = b.cmd("update cancel")
check("boot1: cancel after file staging works",
      b"cancelled" in out and idle_at(b, V0))

out = b.cmd("update stagepkg /sd/PKGNEW.BIN")
check("boot1: stagepkg from /sd stages the signed package",
      b"stagepkg:" in out and b"staged (reboot to trial)" in out)
st = upd_status(b)
check("boot1: /sd stage shows pending_ver=V+1",
      st is not None and st[2] == "staged" and st[3] == V1)
rc = b.poweroff()
check("boot1: clean poweroff rc 0 (trial armed)", rc == 0)

# ================================================================
print("== boot 2: trial + confirm ==")
b = Boot("2")
check("boot2: shell prompt", b.wait_prompt())
check("boot2: TRIAL banner", b.wait("update TRIAL", 60))
check("boot2: CONFIRMED banner", b.wait("update CONFIRMED", 60))
st = upd_status(b)
check("boot2: running V+1 on the flipped slot, idle",
      st is not None and st[0] == V1 and st[1] == SLOT1
      and st[2] == "idle")
b.kill()

# ================================================================
print("== boot 3: yesterday's package is today's downgrade ==")
b = Boot("3")
check("boot3: shell prompt", b.wait_prompt())
out = b.cmd("update stagepkg /sd/PKGNEW.BIN")
check("boot3: same package now rejected as downgrade",
      b"downgrade" in out and idle_at(b, V1))
b.kill()

# ================================================================
print("== cleanup: leave the disk healthy ==")
lfsimg("del", "pkgnew.bin")
fresh_sd()
check("cleanup: /data test file removed, SD image fresh", True)

fails = [n for n, ok in results if not ok]
print(f"FND5-VERIFY: {'PASS' if not fails else 'FAIL'} "
      f"({len(results) - len(fails)}/{len(results)})")
for n in fails:
    print(" - failed:", n)
sys.exit(1 if fails else 0)
