#!/usr/bin/env python3
"""FND4-VERIFY: persistent littlefs backend in QEMU (FND-4).

The /data filesystem (littlefs) must survive a full QEMU
process exit, because its backend is a reserved region of
the blk disk image -- not RAM. Pre-FND-4 the backend was a
RAM array, so every value below evaporated between boots
and this script FAILs at the boot-2 checks.

Setup hygiene (host-side, QEMU stopped): the littlefs
region of the disk image is scrubbed to 0xFF (erased state)
so the script starts from a factory-fresh /data regardless
of what earlier runs left behind. Region constants mirror
lfs_port.c (LFS_REG_TOTAL_RESERVED / geometry): the region
is the 2048 sectors immediately below the bootstate tail
block (16 bootlog sectors + state + scratch = 18 sectors).
Scrubbing is hygiene only; every assertion below is made
from guest-observable evidence (serial shell + HTTP).

Boot A (hygiene): `setup reset` -> provisioning flag clear.
Boot 1: create the web PIN (ONB-1 create flow), POST
  /settings with name/tz + a credstore API key, write a
  marker file, capture the cfg-apikey QSEC blob bytes,
  `setup done`, exit CLEANLY via the `poweroff` builtin
  (QEMU must exit rc 0).
Boot 2 (fresh QEMU process, same image): everything read
  back exactly -- boot reason 4 + "software poweroff" in
  the bootlog, provisioned flag, name/tz/PIN in `setup`,
  marker content, cfg-apikey QSEC blob (visible prefix
  identical + functional clear/re-set proof), login
  with the boot-1 PIN (the login page must no longer be
  the create form), /settings shows the key as present
  and the name rendered. ULFS + UCREDSTORE PASS again.
Boot 3: commit a second marker, then blast the shell with
  put commands and SIGKILL QEMU mid-stream (power cut).
Boot 4: FS still mounts, both committed markers intact,
  provisioning + name intact, bootctl/bootlog parse sane.
  Ends with `setup reset` (battery hygiene).

Harness rules (house): the serial socket is drained
continuously by a reader thread; leftover qemu-system-arm
is killed by exact name first. No GUI is launched: in dev
mode sh owns the console, so serial commands are safe.
"""
import http.client, os, re, socket, subprocess, sys
import threading, time, urllib.parse

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

# littlefs region on the 16 MiB image (see header comment).
TOTAL_SECTORS = 32768
TAIL_RESERVED = 18
REGION_SECTORS = 2048
REGION_BASE = TOTAL_SECTORS - TAIL_RESERVED - REGION_SECTORS

NAME = "fnd4dev"
TZ = "+0930"
PIN = "582047"
SECRET = "fnd4-secret-0001"          # 16 bytes -> 49-byte QSEC blob
MARK1 = "FND4-KEEP-7f3a"
MARK2 = "FND4-SAFE-99"

results = []


def check(name, cond):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name, flush=True)


def scrub_lfs_region():
    with open(IMG, "r+b") as f:
        f.seek(REGION_BASE * 512)
        f.write(b"\xff" * (REGION_SECTORS * 512))


class Boot:
    """One QEMU process + drained serial socket + cmd()."""

    def __init__(self, tag):
        self.tag = tag
        self.sock_path = f"/tmp/fnd4-{tag}.sock"
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
        """Send a shell command; return the segment up to the
        next prompt (segment starts at the command echo)."""
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


def req(method, path, body=None, ck=None):
    c = http.client.HTTPConnection("127.0.0.1", 18080, timeout=8)
    headers = {}
    if body is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if ck:
        headers["Cookie"] = ck
    c.request(method, path, body=body, headers=headers)
    r = c.getresponse()
    data = r.read().decode("utf-8", "replace")
    hdrs = dict(r.getheaders())
    c.close()
    return r.status, hdrs, data


def find_blob(seg):
    """The cfg-apikey QSEC blob as far as shell cat can show
    it: bytes from the QSEC magic up to the first NUL (cat
    is a text command, the blob is binary). That always
    covers the magic + version byte + the head of the
    nonce -- the nonce is random per write, so an equal
    visible prefix across boots means the same sealed
    bytes were read back. The functional checks (boot-2
    PIN login, key-present page, clear + re-set) cover
    the whole-blob round-trip."""
    i = seg.find(b"QSEC")
    if i < 0:
        return None
    j = seg.find(b"\x00", i)
    return seg[i:] if j < 0 else seg[i:j]


# ================================================================
print("== hygiene: scrub littlefs region on the image ==")
scrub_lfs_region()

b = Boot("a")
check("bootA: shell prompt", b.wait_prompt())
out = b.cmd("setup reset")
check("bootA: setup reset cleared", b"cleared" in out)
b.kill()

# ================================================================
print("== boot 1: provision + write ==")
b = Boot("1")
check("boot1: shell prompt", b.wait_prompt())
check("boot1: ULFS PASS (RAM self-test)", b.wait("ULFS: PASS", 30))
check("boot1: UCREDSTORE PASS", b.wait("UCREDSTORE: PASS", 30))

st, hd, body = req("POST", "/login",
                   body=urllib.parse.urlencode({"pin": PIN}))
cookie = hd.get("Set-Cookie", "").split(";")[0]
check("boot1: create PIN -> 303 + session cookie",
      st == 303 and cookie.startswith("qaon_sess="))

st, hd, body = req("POST", "/settings",
                   body=urllib.parse.urlencode(
                       {"name": NAME, "tz": TZ, "mode": "dev",
                        "apikey": SECRET}),
                   ck=cookie)
check("boot1: POST /settings saved (name/tz/apikey)",
      st == 200 and "Tersimpan" in body)

out = b.cmd("setup")
check("boot1: setup shows name + pin set",
      NAME.encode() in out and b"pin: set" in out)

out = b.cmd(f"put /data/fnd4mark.txt {MARK1}")
out = b.cmd("cat /data/fnd4mark.txt")
check("boot1: marker file round-trips in-boot", MARK1.encode() in out)

out = b.cmd("cat /data/cfg-apikey")
blob1 = find_blob(out)
check("boot1: cfg-apikey is a QSEC blob (visible prefix)",
      blob1 is not None and len(blob1) >= 5)

out = b.cmd("setup done")
check("boot1: setup done (provisioned)", b"provisioned" in out)
rc = b.poweroff()
check("boot1: poweroff exits QEMU rc 0", rc == 0)

# ================================================================
print("== boot 2: fresh process, read back exactly ==")
b = Boot("2")
check("boot2: shell prompt", b.wait_prompt())
m = re.search(rb"bootstate: boot #\d+ fail=\d+ reason=(\d+)", b.text())
check("boot2: boot reason = 4 (software poweroff)",
      m is not None and m.group(1) == b"4")
out = b.cmd("bootlog")
check("boot2: bootlog records the software poweroff",
      b"records" in out and b"software poweroff" in b.text())

out = b.cmd("setup")
check("boot2: provisioned + name/tz persisted",
      b"NOT provisioned" not in out and NAME.encode() in out
      and TZ.encode() in out)
check("boot2: PIN persisted (setup shows pin set)",
      b"pin: set" in out)

out = b.cmd("cat /data/fnd4mark.txt")
check("boot2: marker file content exact", MARK1.encode() in out)

out = b.cmd("cat /data/cfg-apikey")
blob2 = find_blob(out)
check("boot2: cfg-apikey visible prefix identical to boot 1",
      blob1 is not None and blob2 == blob1)

st, hd, body = req("GET", "/login")
check("boot2: login page is NOT the create form",
      "belum punya PIN" not in body)
st, hd, body = req("POST", "/login",
                   body=urllib.parse.urlencode({"pin": PIN}))
cookie = hd.get("Set-Cookie", "").split(";")[0]
check("boot2: login with boot-1 PIN -> 303 + cookie",
      st == 303 and cookie.startswith("qaon_sess="))
st, hd, body = req("GET", "/settings", ck=cookie)
check("boot2: /settings shows key present + name rendered",
      st == 200 and "sudah terisi" in body and NAME in body)
# Functional whole-blob proof: clearing the key in boot 2
# must act on the entry boot 1 sealed (present -> absent),
# then re-set it so the disk ends as boot 1 left it.
st, hd, body = req("POST", "/settings",
                   body=urllib.parse.urlencode(
                       {"name": NAME, "tz": TZ, "mode": "dev",
                        "clear_apikey": "1"}), ck=cookie)
st, hd, body = req("GET", "/settings", ck=cookie)
check("boot2: clear removes the persisted boot-1 key",
      st == 200 and "sudah terisi" not in body and
      "kosong" in body)
st, hd, body = req("POST", "/settings",
                   body=urllib.parse.urlencode(
                       {"name": NAME, "tz": TZ, "mode": "dev",
                        "apikey": SECRET}), ck=cookie)
st, hd, body = req("GET", "/settings", ck=cookie)
check("boot2: re-set restores key presence",
      st == 200 and "sudah terisi" in body)
check("boot2: ULFS + UCREDSTORE PASS on persisted storage",
      b.wait("ULFS: PASS", 30) and b.wait("UCREDSTORE: PASS", 30))
rc = b.poweroff()
check("boot2: poweroff exits QEMU rc 0", rc == 0)

# ================================================================
print("== boot 3: commit, then SIGKILL mid-write-stream ==")
b = Boot("3")
check("boot3: shell prompt", b.wait_prompt())
out = b.cmd(f"put /data/fnd4safe.txt {MARK2}")
out = b.cmd("cat /data/fnd4safe.txt")
check("boot3: second marker committed", MARK2.encode() in out)
# Blast puts without waiting for prompts; kill mid-stream.
for i in range(60):
    b.ser.sendall(
        f"put /data/churn{i:02d}.txt cut-churn-{i:02d}-".encode()
        + b"x" * 80 + b"\n")
time.sleep(0.4)
b.kill()   # SIGKILL: the power cut
print("  (QEMU SIGKILLed mid-stream)")

# ================================================================
print("== boot 4: FS mounts, committed data survives ==")
b = Boot("4")
check("boot4: shell prompt (system bootable)", b.wait_prompt())
out = b.cmd("ls /data")
check("boot4: /data mounts and lists after the cut",
      b"mount failed" not in out and b"fnd4safe.txt" in out)
out = b.cmd("cat /data/fnd4safe.txt")
check("boot4: committed marker survives the cut",
      MARK2.encode() in out)
out = b.cmd("cat /data/fnd4mark.txt")
check("boot4: boot-1 marker still intact", MARK1.encode() in out)
out = b.cmd("setup")
check("boot4: provisioning + name survive the cut",
      b"NOT provisioned" not in out and NAME.encode() in out)
out = b.cmd("bootctl")
check("boot4: bootctl sane (fail_count back to 0)",
      b"boot #" in out and b"fail_count=0" in out)
out = b.cmd("bootlog")
check("boot4: bootlog parses (records listed)",
      b"records" in out and b"ev=" in out)
out = b.cmd("setup reset")
check("cleanup: setup reset (leave disk unprovisioned)",
      b"cleared" in out)
b.kill()

# ================================================================
fails = [n for n, ok in results if not ok]
print(f"FND4-VERIFY: {'PASS' if not fails else 'FAIL'} "
      f"({len(results) - len(fails)}/{len(results)})")
for n in fails:
    print(" - failed:", n)
sys.exit(1 if fails else 0)
