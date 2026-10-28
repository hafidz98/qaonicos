"""K4-VERIFY: FND-3 respawn coverage for face (death + hang).

Proves, on one QEMU process with in-place resets (FND-3 pattern):
  A: face killed while FACE holds display -> respawn, eyes again,
     bootlog ev=15, grant works after, BACK releases to eyes.
  B: face killed while UIAPP holds display -> uiapp keeps working,
     pending face request dropped, BACK releases to eyes, grant again.
  C: two deaths in one boot -> respawn 1 + respawn 2, grant works.
  D: third death in one boot -> budget exhausted -> FATAL reset ->
     next boot SAFE MODE -> reboot -> normal boot, fail_count=0,
     face alive after recovery.
  E: face wedged (yield-forever) -> warn -> hang -> hang-respawn,
     grant works (hang path already covered face; this proves it).
  Cleanup: disk unprovisioned, fail_count=0, PIN 246810, name qaon.

Test hook: face polls littlefs "face-kill" ~1 Hz (see user/face/main.c):
  "1" = exit(1) (consumed first), "2" = yield forever (consumed first).
Armed via web shell `put /data/face-kill <v>`; post-GUI shell traffic
goes through the WEB SHELL (FND-3 transport rule: typed serial races
face's console drain once the GUI is up). Key injection goes through
the web shell too (`key menu` -> "key: injected" on serial).

Stale-frame caveat: a screendump after face death still shows the
last flushed frame, so eye/menu pixels alone never prove liveness.
Every respawn check therefore pairs pixels with a liveness signal
only a live face produces: "face: Qabot server starting" and a
working grant (dead face cannot grant).
"""
import http.client, os, re, socket, subprocess, sys
import threading, time, urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.expanduser("~/workspace/logs-k4")
os.makedirs(LOGS, exist_ok=True)
tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"

subprocess.run(["pkill", "-x", "qemu-system-arm"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)

SER = "/tmp/k4-ser.sock"
MON = "/tmp/k4-mon.sock"
for s in (SER, MON):
    try:
        os.unlink(s)
    except FileNotFoundError:
        pass

results = []


def check(name, cond):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name, flush=True)


serial = b""


def start_reader(ser):
    # Tee the full serial transcript to disk as it arrives
    # (crash-proof debugging; the PASS/FAIL log alone cannot
    # explain a surprising check outcome).
    tee = open(os.path.join(LOGS, "verify-k4-serial.log"), "wb")

    def _reader():
        global serial
        while True:
            try:
                d = ser.recv(4096)
                if d:
                    serial += d
                    tee.write(d)
                    tee.flush()
            except socket.timeout:
                continue
            except OSError:
                return
    threading.Thread(target=_reader, daemon=True).start()


def txt():
    return serial.decode("utf-8", "replace")


def wait_from(pos, needle, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle.encode() in serial[pos:]:
            return True
        time.sleep(0.2)
    return needle.encode() in serial[pos:]


def count_from(pos, needle):
    return txt()[pos:].count(needle)


def boot_lines():
    return [(int(n), int(r)) for n, r in re.findall(
        rb"bootstate: boot #(\d+) fail=\d+ reason=(\d+)", serial)]


def wait_new_boot(nprev, timeout=150):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if len(boot_lines()) > nprev:
            return True
        time.sleep(0.3)
    return len(boot_lines()) > nprev


def conn(path):
    for _ in range(75):
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except (ConnectionRefusedError, FileNotFoundError):
                time.sleep(0.2)
        else:
            time.sleep(0.2)
    raise RuntimeError(path)


def req(method, path, body=None, cookie=None):
    c = http.client.HTTPConnection("127.0.0.1", 18080, timeout=6)
    headers = {}
    if body is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if cookie:
        headers["Cookie"] = cookie
    c.request(method, path, body=body, headers=headers)
    r = c.getresponse()
    data = r.read().decode("utf-8", "replace")
    hdrs = dict(r.getheaders())
    c.close()
    return r.status, hdrs, data


p = subprocess.Popen([
    "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
    "-L", tc + "/usr/share/qemu",
    "-kernel", ROOT + "/mach3-port/build/mach3.elf",
    "-serial", f"unix:{SER},server,nowait",
    "-monitor", f"unix:{MON},server,nowait",
    "-device", "virtio-gpu-device", "-display", "none",
    "-drive", f"file={ROOT}/disk-images/mach3-blk.img,if=none,id=hd0,format=raw",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive", f"file={ROOT}/disk-images/sd128.img,if=none,id=hd1,format=raw",
    "-device", "virtio-blk-device,drive=hd1",
    "-netdev", "user,id=net0,hostfwd=tcp::18080-:80",
    "-device", "virtio-net-device,netdev=net0",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)

ser = conn(SER)
ser.settimeout(0.3)
start_reader(ser)
mon = conn(MON)
mon.settimeout(5)
try:
    mon.recv(4096)
except Exception:
    pass


def screendump(name):
    ppm = os.path.join(LOGS, name + ".ppm")
    mon.sendall(f"screendump {ppm}\n".encode())
    time.sleep(1.0)
    try:
        mon.recv(4096)
    except Exception:
        pass
    return ppm


def eye_count(ppm):
    from PIL import Image
    img = Image.open(ppm).convert("RGB")
    cnt = 0
    for yy in range(90, 130):
        for xx in range(40, 200):
            r, g, b = img.getpixel((xx, yy))
            if b > 200 and g > 180 and 100 < r < 200:
                cnt += 1
    return cnt


def blue_count(ppm):
    from PIL import Image
    img = Image.open(ppm).convert("RGB")
    cnt = 0
    for yy in range(0, 240):
        for xx in range(0, 240):
            r, g, b = img.getpixel((xx, yy))
            if b > 150 and b - r > 80:
                cnt += 1
    return cnt


def shell_cmd(cmd, wait_for, timeout=30):
    pos = len(serial)
    ser.sendall(cmd.encode() + b"\n")
    return wait_from(pos, wait_for, timeout)


COOKIE = [None]


def login_and_shell():
    """Web login + settings (web shell on). Returns True on success."""
    try:
        st, hd, _ = req("POST", "/login",
                        body=urllib.parse.urlencode({"pin": "246810"}))
        COOKIE[0] = hd.get("Set-Cookie", "").split(";")[0]
        if not (st == 303 and COOKIE[0].startswith("qaon_sess=")):
            return False
        st, _, _ = req("POST", "/settings",
                       body=urllib.parse.urlencode(
                           {"name": "qaon-k4", "tz": "+0700",
                            "mode": "dev", "shell_on": "on"}),
                       cookie=COOKIE[0])
        return st == 200
    except Exception:
        return False


def web_cmd(cmd):
    """Submit a shell command via the web bridge; output lands on serial."""
    try:
        return req("POST", "/shell",
                   body=urllib.parse.urlencode({"cmd": cmd}),
                   cookie=COOKIE[0])[0]
    except Exception:
        return -1


def reboot_via_web():
    """Reboot through the web shell with a FRESH login first.
    A web session does not survive a reboot, so any reboot issued
    with a pre-reboot cookie is silently rejected (found in the
    pre-change run: phase E's reboot never executed after phase
    D's internal recovery reboot invalidated the cookie)."""
    login_and_shell()
    web_cmd("reboot")


def web_key(k):
    """Inject a button via the web shell; verify "key: injected". Web
    first (no serial bytes to race face's drain); fall back to typed
    serial with one resend (UI5 pattern) if the web path misses."""
    pos = len(serial)
    web_cmd("key " + k)
    if wait_from(pos, "key: injected", 8):
        return True
    pos = len(serial)
    ser.sendall(("key " + k + "\n").encode())
    if wait_from(pos, "key: injected", 8):
        return True
    ser.sendall(("key " + k + "\n").encode())
    return wait_from(pos, "key: injected", 8)


def launch_gui():
    pos = len(serial)
    ser.sendall(b"gui\n")
    return wait_from(pos, "dashboard daemons launched", 60)


def provision():
    """Force setup mode, then complete provisioning to finale eyes.
    Returns (setup_ok, finale_ok). The setup reset MUST run BEFORE
    the first `gui`: a launched face never re-enters setup mode on
    a flag flip (its 1 Hz poll only transitions SETUP -> FINALE),
    it only reads the flag at startup. Fallback retries mirror
    FND-3's loop (typed serial retries: post-GUI bytes race)."""
    pos = len(serial)
    ser.sendall(b"setup reset\n")
    time.sleep(1.0)
    ser.sendall(b"gui\n")
    setup_ok = wait_from(pos, "face: setup mode, payload", 30)
    if not setup_ok:
        for _ in range(4):
            ser.sendall(b"setup reset\n")
            time.sleep(0.6)
            ser.sendall(b"gui\n")
            if wait_from(pos, "face: setup mode, payload", 18):
                setup_ok = True
                break
    if not setup_ok:
        return False, False
    for _ in range(8):
        web_cmd("setup done")
        if wait_from(pos, "provisioned -> finale", 5):
            return True, True
    return True, False


def kill_face():
    """Arm the death marker; wait consumed + exit + death-respawn.
    Returns (consumed, exited, respawned)."""
    pos = len(serial)
    web_cmd("put /data/face-kill 1")
    consumed = wait_from(pos, "face: test kill marker consumed", 10)
    exited = wait_from(pos, "sched: 'face' exit 0x1", 10)
    respawned = wait_from(pos, "watchdog: 'face' death-respawned", 10)
    return consumed, exited, respawned


def bootlog_dump():
    """Full `bootlog` dump, waited until its OWN record count is
    satisfied. Do NOT wait for the "qaon>" prompt: a stale prompt
    from the previous web command can land after pos and satisfy
    it instantly, returning a partial dump. That exact race
    false-passed the phase-A ev=15 check in pre-change run 4:
    the base dump came back near-empty and the after dump then
    "revealed" an ev=15 record left by the wedge repro (an
    earlier boot). The dump header states its own record count,
    so wait for exactly that many "boot #" lines."""
    pos = len(serial)
    web_cmd("bootlog")
    t0 = time.time()
    while time.time() - t0 < 20:
        seg = txt()[pos:]
        m = re.search(r"bootlog: (\d+) records", seg)
        if m:
            n = int(m.group(1))
            got = sum(1 for ln in seg.splitlines()
                      if ln.strip().startswith("boot #"))
            if got >= n:
                return seg
        time.sleep(0.2)
    return txt()[pos:]


def clear_marker():
    """Overwrite /data/face-kill with "0". Race-free only BEFORE
    the phase's `gui`: face has not started yet, so its ~1 Hz
    poll cannot consume a stale arm mid-phase. Called at every
    phase start so no phase depends on the previous phase's
    marker hygiene (a killed run can leave "1" behind)."""
    web_cmd("put /data/face-kill 0")


def grant_menu():
    """Menu key -> uiapp menu renders. Returns (granted, blue)."""
    pos = len(serial)
    if not web_key("menu"):
        return False, 0
    if not wait_from(pos, "UILV2: start", 32):
        return False, 0
    time.sleep(1.0)
    ppm = screendump("k4-menu")
    return True, blue_count(ppm)


def face_eyes():
    time.sleep(2.0)
    ppm = screendump("k4-eyes")
    return eye_count(ppm)


# ================================================================
# Boot 1 + Phase A: death while FACE holds display.
check("boot 1 reaches qaon> prompt", wait_from(0, "qaon>", 150))
check("boot 1 web login + settings (shell on)", login_and_shell())
clear_marker()
# Baseline the persistent bootlog ring BEFORE the kill: the ring
# is not boot-scoped (one dump spans several boots), so the
# respawn check must require a NEW ev=15 record, not just any
# match (an unscoped match false-passed on the hook-only kernel).
base_log = bootlog_dump()
setup_ok, finale_ok = provision()
check("A: GUI launched, face on QR setup screen", setup_ok)
check("A: provisioned to finale (face holds, idle eyes)", finale_ok)
pos = len(serial)
consumed, exited, respawned = kill_face()
check("A: death marker consumed", consumed)
check("A: face exit 0x1 observed", exited)
check("A: face death-respawned (respawn 1)", respawned)
check("A: respawned face starting again",
      wait_from(pos, "face: Qabot server starting", 15))
ec = face_eyes()
print(f"  A: eye pixels after respawn: {ec}", flush=True)
check("A: respawned face renders eyes again", ec > 200)
seg = bootlog_dump()
new_lines = set(seg.splitlines()) - set(base_log.splitlines())
# Scope to THIS boot as well as to newness: the current boot
# number is the newest one in the baseline dump (the current
# boot's own ev=1/ev=2 records are already in the ring). A
# stale record from any earlier boot can then never match,
# even if a dump ever came back partial again.
cur_nums = [int(x) for x in re.findall(r"boot #(\d+)", base_log)]
cur = max(cur_nums) if cur_nums else -1
check("A: bootlog has NEW ev=15 respawn face (this boot)",
      any(ln.strip().startswith("boot #%d " % cur)
          and "ev=15" in ln and "wd respawn face" in ln
          for ln in new_lines))
granted, blue = grant_menu()
print(f"  A: menu blue pixels after respawn: {blue}", flush=True)
check("A: grant works after respawn (menu renders)",
      granted and blue > 1000)
web_key("back")
time.sleep(2.0)
ec = face_eyes()
print(f"  A: eye pixels after BACK release: {ec}", flush=True)
check("A: BACK releases to face eyes again", ec > 200)

# ================================================================
# Boot 2 + Phase B: death while UIAPP holds display + hygiene.
pos = len(serial)
nb = len(boot_lines())
reboot_via_web()
check("B: boot 2 arrives after reboot", wait_new_boot(nb, 150))
check("B: boot 2 qaon> prompt", wait_from(pos, "qaon>", 90))
check("B: boot 2 web login + settings", login_and_shell())
clear_marker()
setup_ok, finale_ok = provision()
check("B: GUI launched, face on QR setup screen", setup_ok)
check("B: provisioned to finale", finale_ok)
granted, blue = grant_menu()
print(f"  B: menu blue pixels (uiapp holds): {blue}", flush=True)
check("B: uiapp menu granted + renders", granted and blue > 1000)
pos = len(serial)
web_cmd("face 1 K4PENDING")
posted = wait_from(pos, "ok", 10)
check("B: face request posted while uiapp holds", posted)
pos = len(serial)
consumed, exited, respawned = kill_face()
check("B: death marker consumed", consumed)
check("B: face exit 0x1 observed", exited)
check("B: face death-respawned (respawn 1)", respawned)
check("B: respawned face starting again",
      wait_from(pos, "face: Qabot server starting", 15))
check("B: pending face request dropped on face death",
      "reclaim: dropped pending face request (face died)"
      in txt()[pos:])
time.sleep(1.0)
ppm = screendump("k4-menu-held")
blue = blue_count(ppm)
print(f"  B: menu blue pixels while face dead: {blue}", flush=True)
check("B: uiapp keeps working while face dead", blue > 1000)
web_key("back")
time.sleep(2.0)
ec = face_eyes()
print(f"  B: eye pixels after BACK release: {ec}", flush=True)
check("B: BACK releases to respawned face eyes", ec > 200)
granted, blue = grant_menu()
check("B: grant works again after respawn", granted and blue > 1000)
web_key("back")
time.sleep(1.0)

# ================================================================
# Boot 3 + Phase C: two deaths in one boot (budget).
pos = len(serial)
nb = len(boot_lines())
reboot_via_web()
check("C: boot 3 arrives after reboot", wait_new_boot(nb, 150))
check("C: boot 3 qaon> prompt", wait_from(pos, "qaon>", 90))
check("C: boot 3 web login + settings", login_and_shell())
clear_marker()
check("C: GUI launched (face in setup)", launch_gui())
time.sleep(3.0)
pos = len(serial)
c1 = kill_face()
check("C: kill 1 -> death-respawned (respawn 1)",
      c1[0] and c1[1] and c1[2] and
      wait_from(pos, "face: Qabot server starting", 15))
pos = len(serial)
c2 = kill_face()
check("C: kill 2 -> death-respawned (respawn 2)",
      c2[0] and c2[1] and c2[2] and
      wait_from(pos, "face: Qabot server starting", 15))
granted, blue = grant_menu()
check("C: grant works after 2 respawns", granted and blue > 1000)
web_key("back")
time.sleep(1.0)

# ================================================================
# Boot 4 + Phase D: third death -> budget exhausted -> FATAL ->
# SAFE MODE -> reboot -> normal boot (self-clear) -> face alive.
pos = len(serial)
nb = len(boot_lines())
reboot_via_web()
check("D: boot 4 arrives after reboot", wait_new_boot(nb, 150))
check("D: boot 4 qaon> prompt", wait_from(pos, "qaon>", 90))
check("D: boot 4 web login + settings", login_and_shell())
clear_marker()
check("D: GUI launched (face in setup)", launch_gui())
time.sleep(3.0)
pos = len(serial)
d1 = kill_face()
check("D: kill 1 -> death-respawned (respawn 1)", d1[2])
pos = len(serial)
d2 = kill_face()
check("D: kill 2 -> death-respawned (respawn 2)", d2[2])
pos = len(serial)
web_cmd("put /data/face-kill 1")
check("D: kill 3 marker consumed",
      wait_from(pos, "face: test kill marker consumed", 10))
check("D: kill 3 -> budget exhausted + FATAL",
      wait_from(pos, "budget exhausted", 15) and
      wait_from(pos, "watchdog: FATAL (face)", 15))
nb = len(boot_lines())
check("D: fatal reset -> new boot", wait_new_boot(nb, 150))
bl = boot_lines()
n2, r2 = bl[-1] if bl else (-1, -1)
print(f"  D: boot after fatal: boot_count={n2} reason={r2}", flush=True)
check("D: boot after fatal has reason=3 (watchdog)", r2 == 3)
check("D: boot after fatal is SAFE MODE", "SAFE MODE" in txt()[pos:])
check("D: safe-mode shell answers",
      shell_cmd("bootctl", "safe_mode=1", 60))
pos = len(serial)
nb = len(boot_lines())
ser.sendall(b"reboot\n")
check("D: reboot from safe mode -> new boot", wait_new_boot(nb, 150))
check("D: recovery boot is NOT safe mode",
      "SAFE MODE" not in txt()[pos:])
check("D: recovery boot reaches qaon> prompt",
      wait_from(pos, "qaon>", 90))
check("D: recovery boot fail_count=0",
      shell_cmd("bootctl", "fail_count=0", 30))
pos = len(serial)
check("D: face alive after recovery (gui -> starting)",
      launch_gui() and
      wait_from(pos, "face: Qabot server starting", 30))

# ================================================================
# Boot 5 + Phase E: wedge -> warn -> hang -> hang-respawn.
pos = len(serial)
nb = len(boot_lines())
reboot_via_web()
check("E: boot 5 arrives after reboot", wait_new_boot(nb, 150))
check("E: boot 5 qaon> prompt", wait_from(pos, "qaon>", 90))
check("E: boot 5 web login + settings", login_and_shell())
clear_marker()
# Self-contained like the standalone repro (which passed on the
# hook-only kernel): provision to finale first, so phase E never
# depends on phase D's leftover provisioned flag or marker state.
setup_ok, finale_ok = provision()
check("E: GUI launched, face on QR setup screen", setup_ok)
check("E: provisioned to finale", finale_ok)
time.sleep(1.0)
pos = len(serial)
web_cmd("put /data/face-kill 2")
check("E: wedge marker consumed",
      wait_from(pos, "face: test wedge marker consumed", 10))
check("E: supervisor warned (face no progress)",
      wait_from(pos, "watchdog: 'face' no progress", 60))
check("E: supervisor declared the hang",
      wait_from(pos, "watchdog: 'face' HANG", 45))
check("E: face hang-respawned (respawn 1)",
      wait_from(pos, "watchdog: 'face' respawned (respawn 1)", 15))
check("E: respawned face starting again",
      wait_from(pos, "face: Qabot server starting", 15))
granted, blue = grant_menu()
check("E: grant works after hang-respawn", granted and blue > 1000)
web_key("back")
time.sleep(1.0)

# ================================================================
# Cleanup: disk unprovisioned, fail_count=0, PIN + name/tz restored.
check("cleanup: web login + settings", login_and_shell())
pos = len(serial)
web_cmd("setup reset")
time.sleep(1.0)
web_cmd("setup status")
check("cleanup: disk NOT provisioned",
      wait_from(pos, "NOT provisioned", 20))
pos = len(serial)
nb = len(boot_lines())
reboot_via_web()
check("cleanup: reboot -> new boot", wait_new_boot(nb, 150))
check("cleanup: boot reaches qaon> prompt",
      wait_from(pos, "qaon>", 90))
check("cleanup: fail_count=0",
      shell_cmd("bootctl", "fail_count=0", 30))
check("cleanup: PIN 246810 still logs in", login_and_shell())
try:
    req("POST", "/settings",
        body=urllib.parse.urlencode(
            {"name": "qaon", "tz": "+0700",
             "mode": "dev", "shell_on": "on"}),
        cookie=COOKIE[0])
except Exception:
    pass

try:
    mon.sendall(b"quit\n")
except Exception:
    pass
time.sleep(1.0)
try:
    p.wait(timeout=10)
except Exception:
    p.kill()

fails = [n for n, ok in results if not ok]
print(f"K4-VERIFY: {'PASS' if not fails else 'FAIL'} "
      f"({len(results) - len(fails)}/{len(results)})", flush=True)
if fails:
    print("Failed checks:", flush=True)
    for n in fails:
        print(f"  FAIL {n}", flush=True)
sys.exit(0 if not fails else 1)
