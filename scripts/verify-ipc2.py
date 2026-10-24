#!/usr/bin/env python3
"""IPC2-VERIFY: blocking receive as a scheduler switch point (IPC-2).

Three boots on the shared dev disk.

Boot 1 (dev mode, typed serial -- sh owns the console):
  - The IPC-2 pair (embed:uipksrv / embed:uipkcli) runs
    the IPC-1 store-and-forward protocol on the BLOCKING
    receive: 100 sequenced messages round-trip with
    byte-exact payloads. The client's setup run opens
    with a deadline probe (500 ms on a silent port ->
    UIPC_ETIME, guest-clock elapsed in range); every
    server drain run ends with a park probe (400 ms on
    the emptied queue -> UIPC_ETIME).
  - Long park: embed:uipkpark parks 65 s on a silent
    port. sh is WAITING on the spawn and the child is
    WAITING on IPC: the all-parked state. Expected: no
    exit for at least 40 s, no new boot (no class-(b)
    reset), then the deadline wakes it ("woke rc=-10",
    exit 0) and the bootlog gained NO watchdog events
    (ev=13/14 counts unchanged across the park).
  - FND-3 adjacency: embed:uyield (yields forever, no
    progress) is still warned, declared hung and killed
    with exit 53405 in the same boot -- a genuinely
    wedged daemon is caught while IPC waits exist.

Boot 2 (GUI up; commands via the web shell):
  - embed:uipknd parks with NO deadline (-1). The
    kernel prints its "parked with no deadline" line
    (the bootlogged form). Mid-park, a screendump must
    still show the face's eyes: face/uiapp kept cycling
    while the child was parked and sh waited. The
    child never wakes in this boot; QEMU is killed
    with the park in place (documented in the dev log:
    the harness has no second concurrent IPC actor --
    the single spawn slot is the parked child itself).

Boot 3 (same disk): the bootlog ring persists on the
blk device, so the no-deadline park's event 17 must be
readable now. Then `setup reset` leaves the disk
unprovisioned and bootctl fail_count=0 (healthy).

Pre-change shape: the kernel ignores the SYS_RECV
timeout argument (IPC-1 poll semantics), so the
deadline probe returns UIPC_EEMPTY at once, both test
programs print FAIL and exit 1, and the park programs
return immediately -- this script FAILs.

Harness rules: the serial socket is drained
continuously by a reader thread (guest printf is
polling-mode); leftover qemu-system-arm is killed by
exact name first; post-`gui` shell commands go through
the web shell (typed serial races the face's console
drain, the FND-3 lesson).
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

SER = "/tmp/ipc2-ser.sock"
MON = "/tmp/ipc2-mon.sock"
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


def boot_lines():
    return [(int(n), int(r)) for n, r in re.findall(
        rb"bootstate: boot #(\d+) fail=\d+ reason=(\d+)", serial)]


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


def launch_qemu():
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
    s = conn(SER)
    s.settimeout(0.3)
    start_reader(s)
    m = conn(MON)
    m.settimeout(5)
    try:
        m.recv(4096)
    except Exception:
        pass
    return p, s, m


p, ser, mon = launch_qemu()


def shell_cmd(cmd, wait_for, timeout=30):
    pos = len(serial)
    ser.sendall(cmd.encode() + b"\n")
    return wait_from(pos, wait_for, timeout)


def run_prog(name, timeout=60):
    """Typed `run embed:<name>` (pre-GUI); returns segment."""
    pos = len(serial)
    ser.sendall(f"run embed:{name}\n".encode())
    wait_from(pos, "exit ", timeout)
    time.sleep(0.3)
    return txt()[pos:]


def bootlog_segment():
    pos = len(serial)
    ser.sendall(b"bootlog\n")
    wait_from(pos, "records", 20)
    time.sleep(0.3)
    return txt()[pos:]


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


# ================= Boot 1 =================
check("boot 1 reaches qaon> prompt", wait_from(0, "qaon>", 150))
check("boot 1 shell alive (bootctl)", shell_cmd("bootctl", "fail_count=", 30))
boots1 = len(boot_lines())

base_log = bootlog_segment()
ev13_base = base_log.count("ev=13")
ev14_base = base_log.count("ev=14")
print(f"  bootlog baseline: ev13={ev13_base} ev14={ev14_base}",
      flush=True)

srv_pass = False
cli_pass = False
seg = run_prog("uipksrv")
check("srv setup run exits 0", "exit 0\n" in seg)
check("srv setup: port published via rendezvous",
      "UIPKSRV: setup port published" in seg)

seg = run_prog("uipkcli")
check("cli setup run exits 0", "exit 0\n" in seg)
check("cli deadline probe: 500 ms park -> ETIME in range",
      "UIPKCLI: deadline-probe ETIME elapsed=" in seg and " OK" in seg)
check("cli setup: batch 0 sent (15 requests queued)",
      "UIPKCLI: progress sent=15 got=0" in seg)

for rnd in range(10):
    if srv_pass and cli_pass:
        break
    if not srv_pass:
        seg = run_prog("uipksrv")
        check(f"srv drain round {rnd} exits 0", "exit 0\n" in seg)
        if "UIPKSRV: PASS served=100/100" in seg:
            srv_pass = True
    if not cli_pass:
        seg = run_prog("uipkcli")
        check(f"cli round {rnd} exits 0", "exit 0\n" in seg)
        if "UIPKCLI: PASS replies=100/100" in seg:
            cli_pass = True

check("server served 100/100 (blocking recv)", srv_pass)
check("client verified 100/100 replies (blocking recv)", cli_pass)
check("server park probes ran inside the flow (ETIME on empty)",
      txt().count("UIPKSRV: park-probe ETIME elapsed=") >= 3)
check("no FAIL marker from either test program",
      "UIPKSRV: FAIL" not in txt() and "UIPKCLI: FAIL" not in txt())

# ---- Long park: the all-parked case ----
pos = len(serial)
ser.sendall(b"run embed:uipkpark\n")
check("park: child entered the 65 s park",
      wait_from(pos, "UIPKPARK: parking deadline=65000", 30))
t_park = time.time()
time.sleep(40)
seg_park = txt()[pos:]
check("park: still parked after 40 s (no early wake, no exit)",
      "UIPKPARK: woke" not in seg_park and "exit " not in seg_park)
check("park: no reset during the park (same boot)",
      len(boot_lines()) == boots1)
check("park: deadline wakes it (rc=-10 ETIME)",
      wait_from(pos, "UIPKPARK: woke rc=-10", 150))
check("park: child exits 0 after the wake",
      wait_from(pos, "exit 0", 20))
print(f"  park wall time: {time.time() - t_park:.1f} s", flush=True)
check("park: program PASS marker", "UIPKPARK: PASS" in txt()[pos:])

after_log = bootlog_segment()
check("park: no NEW watchdog warn (ev=13) from the 65 s park",
      after_log.count("ev=13") == ev13_base)
check("park: no NEW watchdog hang (ev=14) from the 65 s park",
      after_log.count("ev=14") == ev14_base)

# ---- FND-3 adjacency: a wedged child is still caught ----
pos = len(serial)
ser.sendall(b"run embed:uyield\n")
check("adjacency: uyield child started",
      wait_from(pos, "UYIELD: yielding forever", 30))
check("adjacency: supervisor warned on the wedged child",
      wait_from(pos, "watchdog: 'spawned' no progress", 75))
check("adjacency: supervisor declared the hang",
      wait_from(pos, "watchdog: 'spawned' HANG", 60))
check("adjacency: wedged child killed, parent woken 53405",
      wait_from(pos, "exit 53405", 20))
check("boot 1: no kernel fault anywhere", "FAULT" not in txt())
check("boot 1: shell responsive at the end (fail_count=0)",
      shell_cmd("bootctl", "fail_count=0", 30))

# ================= Boot 2 =================
nb = len(boot_lines())
pos = len(serial)
ser.sendall(b"reboot\n")
t0 = time.time()
while len(boot_lines()) <= nb and time.time() - t0 < 120:
    time.sleep(0.3)
check("boot 2 arrives after reboot", len(boot_lines()) > nb)
check("boot 2 reaches qaon> prompt", wait_from(pos, "qaon>", 90))

st, hd, body = req("POST", "/login",
                   body=urllib.parse.urlencode({"pin": "246810"}))
cookie = hd.get("Set-Cookie", "").split(";")[0]
check("boot 2: web login creates PIN + session",
      st == 303 and cookie.startswith("qaon_sess="))
st, hd, body = req("POST", "/settings",
                   body=urllib.parse.urlencode(
                       {"name": "qaon-ipc2", "tz": "+0700",
                        "mode": "dev", "shell_on": "on"}),
                   cookie=cookie)
check("boot 2: web settings enable the web shell", st == 200)


def web_cmd(cmd):
    return req("POST", "/shell",
               body=urllib.parse.urlencode({"cmd": cmd}),
               cookie=cookie)[0]


pos = len(serial)
ser.sendall(b"gui\n")
check("boot 2: GUI daemons launched",
      wait_from(pos, "dashboard daemons launched", 60))
time.sleep(3)

# The face opens on the setup QR while the device is
# unprovisioned and settles on its home (eyes) screen
# after a while; wait for the home screen so the
# mid-park liveness check below samples the same state
# in every run (an earlier version sampled blindly and
# raced the QR screen).
ec = 0
for _try in range(30):
    ec = eye_count(screendump("ipc2-nd-pre"))
    if ec > 200:
        break
    time.sleep(3)
print(f"  face eye pixels before the park: {ec}", flush=True)
check("boot 2: face home screen reached (eyes visible)", ec > 200)

pos = len(serial)
st = web_cmd("run embed:uipknd")
check("boot 2: web shell accepted run embed:uipknd", st == 200)
check("nd: child entered the no-deadline park",
      wait_from(pos, "UIPKND: parking no-deadline", 30))
check("nd: kernel logged the no-deadline park (bootlogged form)",
      wait_from(pos, "parked with no deadline", 30))
time.sleep(6)
ppm = screendump("ipc2-nd-park")
ec = eye_count(ppm)
print(f"  face eye pixels during the no-deadline park: {ec}",
      flush=True)
check("nd: face kept rendering during the park (eyes visible)",
      ec > 200)
seg_nd = txt()[pos:]
check("nd: child still parked (no wake, no exit)",
      "UIPKND: woke" not in seg_nd)
check("nd: no watchdog verdict during the park",
      "watchdog:" not in seg_nd)
p.kill()
time.sleep(1.0)

# ================= Boot 3 =================
for s_ in (SER, MON):
    try:
        os.unlink(s_)
    except FileNotFoundError:
        pass
base3 = len(serial)
p, ser, mon = launch_qemu()
check("boot 3 reaches qaon> prompt", wait_from(base3, "qaon>", 150))
pos = len(serial)
ser.sendall(b"bootlog\n")
wait_from(pos, "records", 20)
time.sleep(0.3)
seg = txt()[pos:]
check("boot 3: bootlog ring carries ev=17 (no-deadline park)",
      "ev=17" in seg)
pos = len(serial)
ser.sendall(b"setup reset\n")
time.sleep(1.0)
check("cleanup: setup reset applied", wait_from(pos, "setup", 20))
pos = len(serial)
ser.sendall(b"setup\n")
check("cleanup: disk left unprovisioned",
      wait_from(pos, "NOT provisioned", 20))
check("cleanup: disk healthy (fail_count=0)",
      shell_cmd("bootctl", "fail_count=0", 30))

p.kill()
with open(os.path.join(ROOT, "logs", "ipc2-serial.log"), "wb") as fh:
    fh.write(serial)
fails = [n for n, ok in results if not ok]
print(f"IPC2-VERIFY: {'PASS' if not fails else 'FAIL'} "
      f"({len(results) - len(fails)}/{len(results)} checks)")
if fails:
    print("failed:", fails)
sys.exit(1 if fails else 0)
