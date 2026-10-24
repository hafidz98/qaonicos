#!/usr/bin/env python3
"""FND3-VERIFY: watchdog / health supervisor (FND-3).

Hang injection through the SYS_SPAWN embed hooks, FND-2
style; the supervisor must detect, escalate and recover
with observable evidence at every rung.

Phase A (class a, yields-but-wedged, basic kill):
  `run embed:uyield` yields forever without progress.
  Expect, in the SAME boot: the warn line + bootlog ev=13,
  the hang + bootlog ev=14, the child killed through the
  reclaim funnel and the parent woken with the watchdog
  exit code ("exit 53405"); boot_count unchanged (no
  reset on the soft rungs).
Phase D (the ladder reaches safe mode, not a loop):
  A clean `run embed:uls` first (resets the consecutive
  kill counter), then three consecutive `run embed:uyield`:
  kills 1-2 wake the shell, the 3rd crosses the budget ->
  fatal watchdog reset -> next boot shows reason=3 AND
  SAFE MODE; the boot after that is normal again (the
  safe boot's healthy mark self-clears the counter).
Phase B2 (class a hang holding the console):
  `run embed:uhangc` takes console ownership and yields
  forever. No idle path can release console ownership,
  so the supervisor kill + FND-2 reclaim is the only way
  back -- deterministic reclaim-under-hang evidence.
Phase B (class a hang holding the display):
  Standard finale flow, then `run embed:uhangd` takes the
  display (PROG_UIAPP hook) and yields forever. The
  supervisor kills it; the token comes home (supervisor
  reclaim, or uiapp's 30 s idle release which starts at
  the same grant -- a true race, reported either way):
  face renders its eyes again, a fresh grant/acquire
  cycle works, the menu renders. Death evidence is the
  watchdog kill + "exit 53405", NOT the 0xdab7 fault path.
Phase C (class b, never yields):
  `run embed:uspin` spins without yielding: no switch
  point ever runs. The timer-IRQ starvation check must
  fire: watchdog reset, next boot reason=3 + SAFE MODE.
Cleanup: reboot until a normal boot with fail_count=0
is observed, so the shared dev disk is left healthy for
the battery (a safe boot self-clears when its one-shots
pass; a flaky one-shot legitimately buys one more).

Harness rules (verify-fnd1/fnd2): the serial socket is
drained continuously by a reader thread (guest printf is
polling-mode); leftover qemu-system-arm is killed by
exact name first.
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

SER = "/tmp/fnd3-ser.sock"
MON = "/tmp/fnd3-mon.sock"
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
    def _reader():
        global serial
        while True:
            try:
                d = ser.recv(4096)
                if d:
                    serial += d
            except socket.timeout:
                pass
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
    """All (boot_count, reason) pairs from kernel boot lines."""
    return [(int(n), int(r)) for n, r in re.findall(
        rb"bootstate: boot #(\d+) fail=\d+ reason=(\d+)", serial)]


def wait_new_boot(nprev, timeout=120):
    """Wait until a boot line beyond the first nprev appears."""
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
    ppm = os.path.join(ROOT, "logs", name + ".ppm")
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


def sel_count(ppm):
    from PIL import Image
    img = Image.open(ppm).convert("RGB")
    cnt = 0
    for yy in range(0, 240):
        for xx in range(0, 240):
            r, g, b = img.getpixel((xx, yy))
            if abs(r - 24) < 14 and abs(g - 45) < 16 and abs(b - 74) < 18:
                cnt += 1
    return cnt


def shell_cmd(cmd, wait_for, timeout=30):
    """Type a shell command on the serial console; wait for a marker."""
    pos = len(serial)
    ser.sendall(cmd.encode() + b"\n")
    return wait_from(pos, wait_for, timeout)


# ================================================================
# Boot 1: Phase A (basic class-a kill) + Phase D (ladder).
# TRANSPORT RULE (FND-2's lesson, sharpened): once the GUI
# is launched, typed serial commands race the face's
# console drain -- in dev mode the face's progid (0)
# equals sh's console-owner slot (0), so the face's event
# poll drains typed bytes and translates them ('m' in
# "embed" becomes EV_MENU -> a spurious grant, and sh
# never sees the full line). Everything after `gui`
# therefore goes through the WEB SHELL, which executes
# the same builtins inside sh with no serial bytes.
check("boot 1 reaches qaon> prompt", wait_from(0, "qaon>", 150))
boots0 = len(boot_lines())
n1 = boot_lines()[-1][0] if boots0 else -1
print(f"  boot 1: boot_count={n1}", flush=True)

st, hd, body = req("POST", "/login",
                   body=urllib.parse.urlencode({"pin": "246810"}))
cookie = hd.get("Set-Cookie", "").split(";")[0]
check("web login creates PIN + session",
      st == 303 and cookie.startswith("qaon_sess="))
st, hd, body = req("POST", "/settings",
                   body=urllib.parse.urlencode(
                       {"name": "qaon-fnd3", "tz": "+0700",
                        "mode": "dev", "shell_on": "on"}),
                   cookie=cookie)
check("web settings enable the web shell", st == 200)


def web_cmd(cmd):
    """Submit a shell command via the web bridge; its
    output still lands on the serial console."""
    return req("POST", "/shell",
               body=urllib.parse.urlencode({"cmd": cmd}),
               cookie=cookie)[0]


pos = len(serial)
ser.sendall(b"gui\n")
check("GUI daemons launched",
      wait_from(pos, "dashboard daemons launched", 60))
time.sleep(3)

# ---------------- Phase A ----------------
pos = len(serial)
web_cmd("run embed:uyield")
check("A: uyield child started",
      wait_from(pos, "UYIELD: yielding forever", 30))
check("A: supervisor warned (no progress)",
      wait_from(pos, "watchdog: 'spawned' no progress", 60))
check("A: supervisor declared the hang",
      wait_from(pos, "watchdog: 'spawned' HANG", 45))
check("A: child killed, parent woken with watchdog code",
      wait_from(pos, "spawn child killed", 15) and
      wait_from(pos, "exit 53405", 15))
check("A: no reset on the soft rungs (same boot)",
      len(boot_lines()) == boots0)
pos = len(serial)
web_cmd("bootlog")
wait_from(pos, "records", 20)
seg = txt()[pos:]
check("A: bootlog has ev=13 (wd warn) + ev=14 (wd hang)",
      "ev=13" in seg and "ev=14" in seg and "wd hang spawned" in seg)
pos = len(serial)
web_cmd("bootctl")
check("A: shell responsive after the kill",
      wait_from(pos, "fail_count=", 20))

# ---------------- Phase D ----------------
pos = len(serial)
web_cmd("run embed:uls")
check("D: clean child run resets the kill counter",
      wait_from(pos, "exit 0", 30))
for k in (1, 2):
    pos = len(serial)
    web_cmd("run embed:uyield")
    ok = wait_from(pos, "exit 53405", 90)
    if not ok:
        print("  D kill-wait segment tail:",
              txt()[pos:][-600:], flush=True)
    check(f"D: consecutive kill {k} wakes the shell", ok)
pos = len(serial)
web_cmd("run embed:uyield")
nb = len(boot_lines())
check("D: 3rd consecutive hang -> watchdog reset (new boot)",
      wait_new_boot(nb, 120))
bl = boot_lines()
n2, r2 = bl[-1] if bl else (-1, -1)
print(f"  boot 2: boot_count={n2} reason={r2}", flush=True)
check("D: boot 2 banner reason=3 (watchdog)", r2 == 3)
# Segment from BEFORE the fatal run: the SAFE MODE banner
# prints before the "bootstate: boot #" summary line, so
# slicing from the boot line itself would exclude it.
check("D: boot 2 is SAFE MODE (3-strikes applied)",
      "SAFE MODE" in txt()[pos:])
check("D: safe-mode shell answers",
      shell_cmd("bootctl", "safe_mode=1", 60))

# Boot 3: the safe boot healthy-marked itself -> normal.
nb = len(boot_lines())
pos = len(serial)
ser.sendall(b"reboot\n")
check("boot 3 arrives after reboot", wait_new_boot(nb, 120))
check("boot 3 is NOT safe mode (self-cleared)",
      "SAFE MODE" not in txt()[pos:])
check("boot 3 reaches qaon> prompt",
      wait_from(pos, "qaon>", 60))
check("boot 3 bootctl fail_count=0",
      shell_cmd("bootctl", "fail_count=0", 20))

# ---------------- Phase B2 (class a hang, console) -------
# Deterministic reclaim-under-hang: nothing but the
# supervisor can release console ownership (no idle path
# exists for it), so the reclaim log line cannot be raced.
pos = len(serial)
ser.sendall(b"run embed:uhangc\n")
check("B2: uhangc child took the console",
      wait_from(pos, "UHANGC: console is mine", 30))
check("B2: supervisor killed the wedged console owner",
      wait_from(pos, "spawn child killed", 75))
check("B2: parent woken with watchdog code",
      wait_from(pos, "exit 53405", 15))
time.sleep(1.0)
check("B2: kernel logged console reclaim",
      "died owning console" in txt()[pos:])
check("B2: console usable again (sh answers bootctl)",
      shell_cmd("bootctl", "fail_count=", 20))

# ---------------- Phase B ----------------
st, hd, body = req("POST", "/login",
                   body=urllib.parse.urlencode({"pin": "246810"}))
cookie = hd.get("Set-Cookie", "").split(";")[0]
check("B: web login creates PIN + session",
      st == 303 and cookie.startswith("qaon_sess="))
st, hd, body = req("POST", "/settings",
                   body=urllib.parse.urlencode(
                       {"name": "qaon-fnd3", "tz": "+0700",
                        "mode": "dev", "shell_on": "on"}),
                   cookie=cookie)
check("B: web settings enable the web shell", st == 200)

pos = len(serial)
qr_ok = False
for _try in range(4):
    ser.sendall(b"setup reset\n")
    time.sleep(0.6)
    ser.sendall(b"gui\n")
    if wait_from(pos, "face: setup mode, payload", 18):
        qr_ok = True
        break
check("B: GUI launched, face on QR setup screen", qr_ok)

finale_ok = False
if qr_ok:
    ser.sendall(b"\n")
    time.sleep(0.5)
    for _attempt in range(8):
        req("POST", "/shell",
            body=urllib.parse.urlencode({"cmd": "setup done"}),
            cookie=cookie)
        if wait_from(pos, "provisioned -> finale", 5):
            finale_ok = True
            break
check("B: finale reached (face holding, idle eyes)", finale_ok)

if finale_ok:
    pos = len(serial)
    ser.sendall(b"\n")
    time.sleep(0.5)
    st, hd, body = req("POST", "/shell",
                       body=urllib.parse.urlencode(
                           {"cmd": "run embed:uhangd"}),
                       cookie=cookie)
    check("B: web shell accepted run embed:uhangd", st == 200)
    check("B: uhangd child became display holder via grant",
          wait_from(pos, "UHANGD: display holder = me", 45))
    check("B: supervisor killed the wedged holder",
          wait_from(pos, "spawn child killed", 75))
    check("B: death was the watchdog path (exit 53405, no fault)",
          wait_from(pos, "exit 53405", 15) and
          "FAULT 0xdab7" not in txt()[pos:])
    time.sleep(1.0)
    # The token comes home by one of two paths and this
    # check accepts either, reporting which: (i) the
    # supervisor's reclaim at kill time, or (ii) uiapp's
    # own 30 s idle auto-release -- the supervisor's 30 s
    # hang deadline for the spawn slot and uiapp's idle
    # window start at the same grant, so which one fires
    # first is a genuine race (both outcomes are safe;
    # the deterministic reclaim-under-hang proof is the
    # console phase B2, where no idle path exists). What
    # must hold either way: the token IS home (the eyes /
    # re-grant / menu checks below).
    via_reclaim = "died holding display" in txt()[pos:]
    via_idle = "uiapp: idle -> release" in txt()[pos:]
    print(f"  token returned via: "
          f"{'supervisor reclaim' if via_reclaim else 'uiapp idle release' if via_idle else 'NEITHER'}",
          flush=True)
    check("B: display token came home (reclaim or idle release)",
          via_reclaim or via_idle)
    grants1 = count_from(0, "[dpy] GRANT -> uiapp")
    acq1 = count_from(0, "uiapp: token acquired")
    time.sleep(2.0)
    ppm = screendump("fnd3-after-kill")
    ec = eye_count(ppm)
    print(f"  eye pixels after holder kill: {ec}", flush=True)
    check("B: face renders its eyes again after reclaim", ec > 200)

    ok_grant = False
    t0 = time.time()
    last_send = 0.0
    while time.time() - t0 < 25:
        if count_from(0, "[dpy] GRANT -> uiapp") > grants1 and \
           count_from(0, "uiapp: token acquired") > acq1:
            ok_grant = True
            break
        if time.time() - last_send > 4.0:
            req("POST", "/shell",
                body=urllib.parse.urlencode({"cmd": "key menu"}),
                cookie=cookie)
            last_send = time.time()
        time.sleep(0.3)
    check("B: face grants again + uiapp re-acquires", ok_grant)
    time.sleep(2.0)
    ppm = screendump("fnd3-menu-again")
    sc = sel_count(ppm)
    print(f"  selected-tile pixels in re-granted menu: {sc}",
          flush=True)
    check("B: menu renders again after re-grant", sc > 800)
else:
    for name in ("B: uhangd child became display holder via grant",
                 "B: supervisor killed the wedged holder",
                 "B: death was the watchdog path (exit 53405, no fault)",
                 "B: display token came home (reclaim or idle release)",
                 "B: face renders its eyes again after reclaim",
                 "B: face grants again + uiapp re-acquires",
                 "B: menu renders again after re-grant"):
        check(name + " [blocked: no finale]", False)

# ---------------- Phase C ----------------
# Via the WEB SHELL, not typed serial: phases B2/B left
# the console open (owner -1 after the reclaim), so a
# typed 17-byte line races qabotd and loses bytes
# (the FND-2 lesson). The bridge runs the same builtin
# inside sh deterministically.
pos = len(serial)
ser.sendall(b"\n")          # flush any partial line in sh
time.sleep(0.5)
st, hd, body = req("POST", "/shell",
                   body=urllib.parse.urlencode(
                       {"cmd": "run embed:uspin"}),
                   cookie=cookie)
check("C: web shell accepted run embed:uspin", st == 200)
check("C: uspin child started spinning",
      wait_from(pos, "USPIN: spinning forever", 30))
nb = len(boot_lines())
check("C: IRQ starvation check reset the device (new boot)",
      wait_new_boot(nb, 120))
bl = boot_lines()
n4, r4 = bl[-1] if bl else (-1, -1)
print(f"  boot 4: boot_count={n4} reason={r4}", flush=True)
check("C: boot 4 banner reason=3 (watchdog)", r4 == 3)
check("C: boot 4 is SAFE MODE",
      "SAFE MODE" in txt()[pos:])
check("C: bootlog carries the watchdog reset (ev=16)",
      shell_cmd("bootlog", "ev=16", 60) or
      "wd reset" in txt())

# Cleanup: reboot until a normal boot with fail_count=0
# is observed, leaving the shared disk healthy for the
# battery. A safe-mode boot whose one-shots pass marks
# healthy and self-clears; but a boot with a flaky
# one-shot (observed in the wild: ufs/FAT under host
# contention) legitimately skips the mark and buys one
# more safe boot -- bounded here to two extra boots.
clean = False
for _extra in range(3):
    nb = len(boot_lines())
    pos = len(serial)
    ser.sendall(b"reboot\n")
    if not wait_new_boot(nb, 120):
        break
    if "SAFE MODE" in txt()[pos:]:
        # Safe again: fine if THIS boot healthy-marked
        # (fail_count back to 0); the next one is normal.
        shell_cmd("bootctl", "fail_count=", 30)
        continue
    if shell_cmd("bootctl", "fail_count=0", 30):
        clean = True
        break
check("cleanup: disk left healthy (normal boot, fail_count=0)",
      clean)

p.kill()
with open(os.path.join(ROOT, "logs", "fnd3-serial.log"), "wb") as fh:
    fh.write(serial)
fails = [n for n, ok in results if not ok]
print(f"FND3-VERIFY: {'PASS' if not fails else 'FAIL'} "
      f"({len(results) - len(fails)}/{len(results)} checks)")
if fails:
    print("failed:", fails)
sys.exit(1 if fails else 0)
