#!/usr/bin/env python3
"""FND2-VERIFY: resource reclamation on daemon death (FND-2).

Fault injection through real product paths only: a test
child is spawned with SYS_SPAWN (the shell's `run`), dies
on a deliberate data abort, and the kernel's isolated-fault
path (user_fault -> sched_exit_switch) must hand back what
it held.

Phase A (console + web-shell bridge):
  A child takes console ownership (the state sh normally
  holds), takes a pending web-shell command (bridge ->
  running) and faults without posting. Afterwards:
  - the kernel logs the console reclaim;
  - the bridge is settled: the web side polling the result
    gets the "executor exited" error instead of a wedge;
  - sh answers a typed command again (pre-fix the console
    owner index points at the dead child forever and
    SYS_READ_CONSOLE refuses sh -> total silence).
Phase B (display token):
  Standard flow to the finale, then a child carrying the
  holder progid (embed hook scaffolding; the token is
  progid-keyed) is granted the display by the face via
  EV_MENU, acquires it, and faults. Afterwards:
  - the kernel logs the display reclaim;
  - the face renders its eyes again (screendump pixel
    check, the ONB2 heuristic);
  - a fresh `key menu` makes the face grant again
    ("[dpy] GRANT -> uiapp" + "uiapp: token acquired"
    appear a second time) and the menu renders.
  All post-death checks run well inside uiapp's 30 s idle
  auto-release window so a pre-fix kernel cannot pass them
  by timing out the token.

Harness rules (see verify-onb1.py / verify-fnd1.py): the
serial socket is drained continuously by a reader thread
(guest printf is polling-mode); leftover qemu-system-arm
is killed by exact name first.
"""
import http.client, os, re, socket, subprocess, sys
import threading, time, urllib.parse

ROOT = "/home/hatch/workspace/qaonic_os"
tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"

subprocess.run(["pkill", "-x", "qemu-system-arm"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)

SER = "/tmp/fnd2-ser.sock"
MON = "/tmp/fnd2-mon.sock"
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
    """Pixels of the selected-tile fill C_SEL (RGB ~ (24,45,74))."""
    from PIL import Image
    img = Image.open(ppm).convert("RGB")
    cnt = 0
    for yy in range(0, 240):
        for xx in range(0, 240):
            r, g, b = img.getpixel((xx, yy))
            if abs(r - 24) < 14 and abs(g - 45) < 16 and abs(b - 74) < 18:
                cnt += 1
    return cnt


# ----------------------------------------------------------------
# Phase A: console ownership + web-shell bridge settlement.
# (Skipped in "--only-b" mode, used to demonstrate the display
# wedge independently in a pre-fix kernel: there phase A's
# console wedge blocks phase B from even starting.)
ONLY_B = "--only-b" in sys.argv
cookie = None
check("boot reaches qaon> prompt", wait_from(0, "qaon>", 150))

if not ONLY_B:
    st, hd, body = req("POST", "/login",
                       body=urllib.parse.urlencode({"pin": "246810"}))
    cookie = hd.get("Set-Cookie", "").split(";")[0]
    check("web login creates PIN + session", st == 303 and
          cookie.startswith("qaon_sess="))

    st, hd, body = req("POST", "/settings",
                       body=urllib.parse.urlencode(
                           {"name": "qaon-fnd2", "tz": "+0700",
                            "mode": "dev", "shell_on": "on"}),
                       cookie=cookie)
    check("web settings enable the web shell", st == 200)

    pos = len(serial)
    ser.sendall(b"run embed:ufault\n")
    check("ufault child took the console",
          wait_from(pos, "UFAULT: console is mine", 30))

    st, hd, body = req("POST", "/shell",
                       body=urllib.parse.urlencode({"cmd": "bootctl"}),
                       cookie=cookie)
    check("web shell command submitted", st == 200)
    check("ufault child took the web-shell command",
          wait_from(pos, "UFAULT: took web-shell command", 25))
    check("child fault isolated (FAULT 0xdab7)",
          wait_from(pos, "FAULT 0xdab7", 15))
    check("spawn returned the fault code to sh",
          wait_from(pos, "exit 55991", 15))
    time.sleep(1.0)
    check("kernel logged console reclaim",
          "died owning console" in txt()[pos:])

    settled = False
    for _ in range(10):
        st, hd, body = req("GET", "/shell", cookie=cookie)
        if "executor exited" in body:
            settled = True
            break
        time.sleep(2)
    check("web-shell bridge settled with executor error", settled)

    pos2 = len(serial)
    ser.sendall(b"bootctl\n")
    check("console usable again: sh answers bootctl",
          wait_from(pos2, "last_reason=", 20))

# ----------------------------------------------------------------
# Phase B: display token reclaim.
# Console note: phase A left the console OPEN (the reclaim
# restored the boot default), so qabotd -- launched by `gui`
# below -- competes with sh for typed lines and wins some of
# them (pre-existing Q9 semantics; the same quirk the video
# recorder works around). Phase B therefore re-sends each
# command until its acknowledgement appears.
pos = len(serial)
qr_ok = False
for _try in range(4):
    ser.sendall(b"setup reset\n")
    time.sleep(0.6)
    ser.sendall(b"gui\n")
    if wait_from(pos, "face: setup mode, payload", 18):
        qr_ok = True
        break
check("GUI launched, face on QR setup screen", qr_ok)
if cookie is None:
    # --only-b mode: phase A never logged in; the web shell
    # steps below need a session + an enabled shell. (In
    # full mode phase A already did this.)
    st, hd, body = req("POST", "/login",
                       body=urllib.parse.urlencode(
                           {"pin": "246810"}))
    cookie = hd.get("Set-Cookie", "").split(";")[0]
    req("POST", "/settings",
        body=urllib.parse.urlencode(
            {"name": "qaon-fnd2", "tz": "+0700",
             "mode": "dev", "shell_on": "on"}),
        cookie=cookie)

finale_ok = False
if qr_ok:
    # "setup done" goes through the WEB SHELL: typed on the
    # serial line it races qabotd for the open console and
    # loses persistently in some boots (split lines also
    # mangle sh's line buffer). The bridge runs the same
    # builtin inside sh, deterministically.
    ser.sendall(b"\n")          # flush any partial line in sh
    time.sleep(0.5)
    for _attempt in range(8):
        req("POST", "/shell",
            body=urllib.parse.urlencode({"cmd": "setup done"}),
            cookie=cookie)
        if wait_from(pos, "provisioned -> finale", 5):
            finale_ok = True
            break
check("finale reached (face holding, idle eyes)", finale_ok)

if finale_ok:
    pos = len(serial)
    grants0 = count_from(0, "[dpy] GRANT -> uiapp")
    # Spawn the holder child through the WEB SHELL, not the
    # serial line: at 18 bytes "run embed:ufaultd" loses the
    # open-console byte race against qabotd almost every
    # time (observed: 5/5 attempts eaten while shorter
    # commands got through). The web bridge executes the
    # same `run` builtin inside sh, no serial bytes needed.
    ser.sendall(b"\n")          # flush any partial line in sh
    time.sleep(0.5)
    st, hd, body = req("POST", "/shell",
                       body=urllib.parse.urlencode(
                           {"cmd": "run embed:ufaultd"}),
                       cookie=cookie)
    check("web shell accepted run embed:ufaultd", st == 200)
    check("ufaultd child became display holder via grant",
          wait_from(pos, "UFAULTD: display holder = me", 45))
    check("holder child fault isolated",
          wait_from(pos, "FAULT 0xdab7", 15))
    check("spawn returned the fault code (display)",
          wait_from(pos, "exit 55991", 15))
    time.sleep(1.0)
    check("kernel logged display reclaim",
          "died holding display" in txt()[pos:])
    # Snapshot AFTER the test child's own grant: the re-grant
    # check below must require a NEW grant + acquire cycle.
    grants1 = count_from(0, "[dpy] GRANT -> uiapp")
    acq1 = count_from(0, "uiapp: token acquired")
    time.sleep(2.0)
    ppm = screendump("fnd2-after-death")
    ec = eye_count(ppm)
    print(f"  eye pixels after holder death: {ec}", flush=True)
    check("face renders its eyes again after reclaim", ec > 200)

    ok_grant = False
    t0 = time.time()
    last_send = 0.0
    while time.time() - t0 < 25:
        if count_from(0, "[dpy] GRANT -> uiapp") > grants1 and \
           count_from(0, "uiapp: token acquired") > acq1:
            ok_grant = True
            break
        if time.time() - last_send > 4.0:
            # Inject via the web shell, not serial bytes:
            # with the console open, a typed "key menu" line
            # races qabotd/face-drain and loses too often.
            req("POST", "/shell",
                body=urllib.parse.urlencode({"cmd": "key menu"}),
                cookie=cookie)
            last_send = time.time()
        time.sleep(0.3)
    check("face grants again + uiapp re-acquires", ok_grant)
    time.sleep(2.0)
    ppm = screendump("fnd2-menu-again")
    sc = sel_count(ppm)
    print(f"  selected-tile pixels in re-granted menu: {sc}",
          flush=True)
    check("menu renders again after re-grant", sc > 800)
else:
    for name in ("ufaultd child became display holder via grant",
                 "holder child fault isolated",
                 "spawn returned the fault code (display)",
                 "kernel logged display reclaim",
                 "face renders its eyes again after reclaim",
                 "face grants again + uiapp re-acquires",
                 "menu renders again after re-grant"):
        check(name + " [blocked: no finale]", False)

p.kill()
fails = [n for n, ok in results if not ok]
print(f"FND2-VERIFY: {'PASS' if not fails else 'FAIL'} "
      f"({len(results) - len(fails)}/{len(results)} checks)")
if fails:
    print("failed:", fails)
sys.exit(1 if fails else 0)
