"""K5-VERIFY: live radio-state wiring for the status-bar icons.

The status bar shows Material Symbols wifi/BT glyphs (UI-8), but
the call site passed (0,0) forever: icons always OFF. K5 wires
linkd's live WIFI.STATUS / BLE.STATUS through a poll/cache in
the uiapp shell loop (LVGL timer, 5 s period, first fire one
period after menu build -- the menu-build path itself never
RPCs, so menu appearance costs zero RPCs even with linkd dead).

Proof is VISUAL (screendump icon-cell evidence, UI-8 style),
not log lines alone: the wifi/BT icon cells (wifi 172..190,
bluetooth 194..212, rows 0..23) hold measurably different
icon-gray counts for the ON glyph vs the OFF glyph, against
the UI-8 OFF baselines (wifi-gray=120, bt-gray=77).

Phases (one boot, one peer, driver = QA program `uradio`
run from the shell, marker `radio-cmd` via `put` + `cat`
read-back, C6L-5 pattern). The peer starts ON/ON
(--wifi-on WARUNGKOPI --ble-on); the run first drives
it OFF/OFF for a reference dump, then:
  1. drive ON/ON -> icons ON/ON. On the pre-change
     kernel this FAILS (icons never move) -- that is
     the test-first evidence.
  2. drive bt OFF -> icons ON/OFF.
  3. drive wifi OFF, bt ON -> icons OFF/ON.
  4. drive bt OFF -> icons OFF/OFF (baseline).
  5. peer STOPPED (no peer at all): after 2+ poll
     periods icons must be OFF/OFF (failure -> OFF,
     never stale ON) AND the menu must still respond
     to keys (no poll wedge). Menu grant time is
     measured with peer up and peer dead (re-grant)
     and both must be < 25 s.
A focus poke (down+up) precedes every wait: the
shell loop idle-exits after 30 s without keys, and
an idle-exited menu would dump face instead.
Teardown restores name qaon + tz +0700 + unprovisioned
(UI-5 pattern); PIN 246810 is never touched.

Harness rules: serial socket drained continuously by a
reader thread; leftover qemu-system-arm killed by exact
name first; the peer is never left running. Scratch
lives under ~/workspace/logs-k5 (NOT /tmp).
"""
import os
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get("KERNEL", ROOT + "/mach3-port/build/mach3.elf")
PEER = os.environ.get("PEER", os.path.join(ROOT, "scripts", "mock-c6.py"))
PORT = 18082
LOGDIR = os.path.expanduser("~/workspace/logs-k5")
os.makedirs(LOGDIR, exist_ok=True)

tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"

subprocess.run(["pkill", "-x", "qemu-system-arm"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)

results = []


def check(name, cond):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name, flush=True)


def start_peer(tag, extra=None):
    tr = os.path.join(LOGDIR, f"k5-{tag}-transcript.log")
    st = os.path.join(LOGDIR, f"k5-{tag}-stats.json")
    for f in (tr, st):
        try:
            os.unlink(f)
        except FileNotFoundError:
            pass
    args = [sys.executable, PEER, "--port", str(PORT),
            "--transcript", tr, "--stats-file", st] + (extra or [])
    p = subprocess.Popen(args,
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < 15:
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=1)
            s.close()
            return p
        except OSError:
            if p.poll() is not None:
                break
            time.sleep(0.2)
    raise RuntimeError("peer did not start listening")


def stop_peer(p):
    try:
        p.terminate()
        p.wait(timeout=5)
    except Exception:
        try:
            p.kill()
        except Exception:
            pass
    time.sleep(0.5)


SER = os.path.join(LOGDIR, "k5-ser.sock")
MON = os.path.join(LOGDIR, "k5-mon.sock")
for s in (SER, MON):
    try:
        os.unlink(s)
    except FileNotFoundError:
        pass

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


def wait_from(pos, needle, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle.encode() in serial[pos:]:
            return True
        time.sleep(0.2)
    return needle.encode() in serial[pos:]


def find_after(pos, needle):
    return needle.encode() in serial[pos:]


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


def launch_qemu():
    for s in (SER, MON):
        try:
            os.unlink(s)
        except FileNotFoundError:
            pass
    p = subprocess.Popen([
        "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
        "-L", tc + "/usr/share/qemu",
        "-kernel", KERNEL,
        "-serial", f"unix:{SER},server,nowait",
        "-monitor", f"unix:{MON},server,nowait",
        "-device", "virtio-gpu-device", "-display", "none",
        "-drive", f"file={ROOT}/disk-images/mach3-blk.img,if=none,id=hd0,format=raw",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"file={ROOT}/disk-images/sd128.img,if=none,id=hd1,format=raw",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=net0,hostfwd=tcp::18083-:80",
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


def shell_cmd(ser, cmd, wait_for, timeout=30):
    # C6L-5 lost-input rule: retry ONLY when the
    # command echo is absent (input never arrived);
    # an echo present means the command ran.
    cb = cmd.encode()
    for _ in range(3):
        pos = len(serial)
        ser.sendall(cb + b"\n")
        if wait_from(pos, wait_for, timeout):
            return True
        seg = serial[pos:]
        if b"\n" + cb in seg or b"qaon> " + cb in seg:
            return False
    return False


def send_key(ser, k):
    """One key with injection verification (resend once)."""
    for _ in range(2):
        pos = len(serial)
        ser.sendall(f"key {k}\n".encode())
        if wait_from(pos, "key: injected", 5):
            return True
    return False


def key(ser, k, needle, timeout=32):
    for _ in range(2):
        pos = len(serial)
        ser.sendall(f"key {k}\n".encode())
        if wait_from(pos, "key: injected", 5):
            break
    return wait_from(pos, needle, timeout)


def grant_menu_timed(ser):
    """Fresh token grant -> shell menu, with retries
    (UI-5 pattern). Returns grant->start seconds, or
    -1 on failure."""
    for _ in range(3):
        pos = len(serial)
        t0 = time.time()
        ser.sendall(b"key menu\n")
        if not wait_from(pos, "key: injected", 5):
            pos = len(serial)
            t0 = time.time()
            ser.sendall(b"key menu\n")
            if not wait_from(pos, "key: injected", 5):
                continue
        if wait_from(pos, "UILV2: start", 32):
            return time.time() - t0
        if find_after(pos, "UILV2: focus"):
            return time.time() - t0
    pos = len(serial)
    send_key(ser, "back")
    if wait_from(pos, "UILV2: start", 32):
        return -1.0  # recovered oddly; not a clean timing
    wait_from(pos, "UILV2: release", 32)
    pos = len(serial)
    t0 = time.time()
    ser.sendall(b"key menu\n")
    if not wait_from(pos, "key: injected", 5):
        return -1
    if wait_from(pos, "UILV2: start", 32):
        return time.time() - t0
    return -1


def setup_flow(ser):
    """UI-5 pattern: setup reset -> gui -> setup done."""
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(0.6)
    ser.sendall(b"gui\n")
    if not wait_from(0, "face: setup mode, payload", 60):
        return False
    if not shell_cmd(ser, "setup status", "  name: ", 15):
        return False
    for _ in range(4):
        pos = len(serial)
        ser.sendall(b"setup done\n")
        t0 = time.time()
        while time.time() - t0 < 15:
            seg = serial[pos:]
            if (b"setup: done" in seg or
                    b"provisioned -> finale" in seg):
                return True
            time.sleep(0.2)
    return False


def release_to_shell(ser):
    send_key(ser, "back")
    time.sleep(1.0)
    send_key(ser, "back")
    time.sleep(1.0)
    send_key(ser, "back")
    time.sleep(1.5)


def set_marker(ser, val, tries=3):
    # C6L-5 rule: put + cat read-back must agree;
    # a garbled line can report "put: ok" while
    # writing a garbled name.
    want = "\n" + val + "\n"
    for _ in range(tries):
        if not shell_cmd(ser, "put /data/radio-cmd " + val,
                         "put: ok", 20):
            continue
        if shell_cmd(ser, "cat /data/radio-cmd", want, 20):
            return True
    return False


def run_spawn(ser, cmd, start_marker, timeout=60, tries=3):
    # C6L-5 rule: retry ONLY on "spawn gagal"
    # (a failed spawn leaves the slot free, so a
    # retry cannot double-start); any other output
    # is a real signal. Poll for BOTH markers at
    # once: waiting the full timeout before noticing
    # a fast "spawn gagal" burns 60 s per attempt
    # (pre-change-run lesson: 7 drives x 3 tries).
    for _ in range(tries):
        pos = len(serial)
        ser.sendall(cmd.encode() + b"\n")
        t0 = time.time()
        while time.time() - t0 < timeout:
            seg = serial[pos:]
            if start_marker.encode() in seg:
                return True
            if b"spawn gagal" in seg:
                break
            time.sleep(0.2)
        else:
            return False
    return False


def drive(ser, line):
    """Drive one radio command through uradio.
    Returns the URADIO result line for THIS command,
    or None. The search is for the exact line: a
    later "URADIO: clear fail" warning (or any other
    URADIO line) must never stand in for the result.
    NOTE: no-payload commands (WIFI.DISC, BLE.ON,
    BLE.OFF) return an EMPTY response body -- the
    wire "OK" is the status word linkd strips -- so
    their result line ends with "-> " (empty), never
    "-> OK". A failed command ends with "-> ERR"."""
    if not set_marker(ser, line):
        return None
    pos = len(serial)
    if not run_spawn(ser, "run embed:uradio", "URADIO: ", 60):
        return None
    seg = serial[pos:].decode("utf-8", "replace")
    want = "URADIO: " + line + " -> "
    i = seg.find(want)
    if i < 0:
        return None
    return seg[i:].split("\n")[0]


def poke_menu(ser):
    """Reset the shell loop's 30 s idle timer: a
    down+up focus wiggle (returns to the same entry
    unless focus wraps, which is harmless here).
    Without it, phases past ~30 s after the last key
    idle-exit to face and their dumps show no menu
    at all (measurement-run lesson)."""
    send_key(ser, "down")
    time.sleep(0.3)
    send_key(ser, "up")
    time.sleep(0.3)


def screendump(mon, name):
    ppm = os.path.join(LOGDIR, name + ".ppm")
    mon.sendall(f"screendump {ppm}\n".encode())
    time.sleep(1.0)
    try:
        mon.recv(4096)
    except Exception:
        pass
    return ppm if os.path.exists(ppm) else None


def dump_menu(mon, ser, name):
    """Screendump that must show the menu. If the
    menu idle-exited (a 30 s race the measurement
    run hit: drives between pokes can exceed the
    idle budget under serial load), the dump shows
    face instead -- re-grant once, wait past a poll
    period, and re-dump. Returns the dump path."""
    ppm = screendump(mon, name)
    if ppm is not None and bar_light(ppm) > 1000:
        return ppm
    t = grant_menu_timed(ser)
    print(f"  re-grant for {name}: {t:.1f}s", flush=True)
    poke_menu(ser)
    time.sleep(8.0)
    return screendump(mon, name + "-regrant")


def _is_icon_gray(px):
    r, g, b = px[0], px[1], px[2]
    return 100 < r < 200 and max(r, g, b) - min(r, g, b) < 30


def icon_counts(ppm):
    """(wifi, bt, batt) icon-gray counts in the UI-8
    boxes: wifi 172..190, bluetooth 194..212,
    battery 216..234, rows 0..23."""
    from PIL import Image
    im = Image.open(ppm).convert("RGB")

    def count(x0, x1):
        return sum(1 for x in range(x0, x1 + 1)
                   for y in range(0, 24)
                   if _is_icon_gray(im.getpixel((x, y))))

    batt_empty = not _is_icon_gray(im.getpixel((225, 11)))
    return count(172, 190), count(194, 212), count(216, 234), batt_empty


# UI-8 OFF baselines (same boxes, same font),
# printed for the record only. ON/OFF discrimination
# is NOT by total gray count: the BT ON glyph and the
# BT OFF glyph both total 77 gray pixels (different
# row shapes, same total -- measurement-run lesson).
# Discrimination is by cell pixel-diff against a
# known-OFF reference dump from the same run.
# BATT_OFF = 78 (outline-only battery, UI-8).
BATT_OFF = 78


def cell_diff(a, b, x0, x1):
    """Differing pixels between two dumps in one
    icon cell (rows 0..23)."""
    from PIL import Image
    im_a = Image.open(a).convert("RGB")
    im_b = Image.open(b).convert("RGB")
    return sum(1 for x in range(x0, x1 + 1)
               for y in range(24)
               if im_a.getpixel((x, y)) != im_b.getpixel((x, y)))


def bar_light(ppm):
    """Light-gray pixels in the status-bar band
    (rows 0-20): the menu bar background is flat
    light gray (0xd8d8d8), thousands of pixels on
    every menu screen. The face screen's top band
    is dark navy instead (~0) -- so this, not a
    dark-pixel count, discriminates menu from face.
    (A dark-pixel count scores face HIGHER than
    menu: the measurement-run lesson behind this
    function.)"""
    from PIL import Image
    im = Image.open(ppm).convert("RGB")

    def is_bar(px):
        r, g, b = px[0], px[1], px[2]
        return (200 <= r <= 235 and 200 <= g <= 235 and
                200 <= b <= 235 and max(px) - min(px) < 30)

    return sum(1 for y in range(20) for x in range(240)
               if is_bar(im.getpixel((x, y))))


def state_ok(ppm, ref, want_wifi, want_bt):
    """want_* is 1 (ON) or 0 (OFF) relative to the
    OFF reference dump `ref`: ON = cell differs by
    > 20 pixels (a different glyph), OFF = cell
    differs by < 10 (the same glyph). Both cells must
    also hold a glyph (wifi >= 25, bt >= 15 gray) and
    the battery must stay outline-only (count within
    30 of 78, interior empty)."""
    if ppm is None or ref is None:
        return False, "no dump"
    w, b, t, be = icon_counts(ppm)
    txt = (f"wifi-gray={w} bt-gray={b} batt-gray={t} batt-empty={be} "
           f"wifi-diff={cell_diff(ppm, ref, 172, 190)} "
           f"bt-diff={cell_diff(ppm, ref, 194, 212)}")
    if not be or abs(t - BATT_OFF) > 30:
        return False, txt + " BATT-BAD"
    if w < 25 or b < 15:
        return False, txt + " GLYPH-MISSING"
    if bar_light(ppm) < 1000:
        return False, txt + " NO-MENU"
    wd = cell_diff(ppm, ref, 172, 190)
    bd = cell_diff(ppm, ref, 194, 212)
    wok = (wd > 20) if want_wifi else (wd < 10)
    bok = (bd > 20) if want_bt else (bd < 10)
    return (wok and bok), txt


peer = None
qemu = None
ser = mon = None
try:
    # Phase 1 peer starts already ON/ON: the
    # pre-change kernel must FAIL here (icons OFF).
    peer = start_peer("on", ["--wifi-on", "WARUNGKOPI", "--ble-on"])
    qemu, ser, mon = launch_qemu()

    check("boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    check("setup flow reaches finale", setup_flow(ser))
    check("shell sets timezone +0700",
          shell_cmd(ser, "setup tz +0700", "setup: tz saved", 10))

    # ---- Grant (peer up) + OFF reference ----
    # The peer starts ON/ON; drive it OFF/OFF first
    # so every later check compares against a
    # known-OFF reference dump from this same run.
    # On the pre-change kernel the ON phases below
    # FAIL (icons never move) -- test-first evidence.
    t_grant = grant_menu_timed(ser)
    print(f"  grant->menu (peer up): {t_grant:.1f}s", flush=True)
    check("grant starts the shell menu (peer up)", t_grant >= 0)
    check("menu appears fast with peer up (< 25 s)",
          0 <= t_grant < 25)
    poke_menu(ser)
    line = drive(ser, "WIFI.DISC")
    print(f"  drive: {line}", flush=True)
    check("drive WIFI.DISC ok",
          line is not None and line.endswith("-> "))
    line = drive(ser, "BLE.OFF")
    print(f"  drive: {line}", flush=True)
    check("drive BLE.OFF ok",
          line is not None and line.endswith("-> "))
    poke_menu(ser)
    time.sleep(8.0)  # past the 5 s poll + margin
    ppm_ref = dump_menu(mon, ser, "k5-ref-off")
    check("reference OFF screendump saved", ppm_ref is not None)
    check("reference dump shows a menu (bar renders)",
          ppm_ref is not None and bar_light(ppm_ref) > 1000)

    # ---- Phase 1: drive ON/ON ----
    line = drive(ser, "WIFI.CONNECT WARUNGKOPI")
    print(f"  drive: {line}", flush=True)
    check("drive WIFI.CONNECT ok",
          line is not None and "CONNECTED" in line)
    line = drive(ser, "BLE.ON")
    print(f"  drive: {line}", flush=True)
    check("drive BLE.ON ok",
          line is not None and line.endswith("-> "))
    poke_menu(ser)
    time.sleep(8.0)
    ppm = dump_menu(mon, ser, "k5-on-on")
    ok, txt = state_ok(ppm, ppm_ref, 1, 1)
    print(f"  phase 1 icons: {txt}", flush=True)
    check("phase 1: drive ON/ON -> icons ON/ON", ok)

    # ---- Phase 2: drive bt OFF -> ON/OFF ----
    line = drive(ser, "BLE.OFF")
    print(f"  drive: {line}", flush=True)
    check("drive BLE.OFF (2) ok",
          line is not None and line.endswith("-> "))
    poke_menu(ser)
    time.sleep(8.0)
    ppm = dump_menu(mon, ser, "k5-on-off")
    ok, txt = state_ok(ppm, ppm_ref, 1, 0)
    print(f"  phase 2 icons: {txt}", flush=True)
    check("phase 2: bt OFF -> icons ON/OFF", ok)

    # ---- Phase 3: drive wifi OFF, bt ON -> OFF/ON ----
    line = drive(ser, "WIFI.DISC")
    print(f"  drive: {line}", flush=True)
    check("drive WIFI.DISC (2) ok",
          line is not None and line.endswith("-> "))
    line = drive(ser, "BLE.ON")
    print(f"  drive: {line}", flush=True)
    check("drive BLE.ON (2) ok",
          line is not None and line.endswith("-> "))
    poke_menu(ser)
    time.sleep(8.0)
    ppm = dump_menu(mon, ser, "k5-off-on")
    ok, txt = state_ok(ppm, ppm_ref, 0, 1)
    print(f"  phase 3 icons: {txt}", flush=True)
    check("phase 3: wifi OFF, bt ON -> icons OFF/ON", ok)

    # ---- Phase 4: back to OFF/OFF ----
    line = drive(ser, "BLE.OFF")
    print(f"  drive: {line}", flush=True)
    check("drive BLE.OFF (3) ok",
          line is not None and line.endswith("-> "))
    poke_menu(ser)
    time.sleep(8.0)
    ppm = dump_menu(mon, ser, "k5-off-off")
    ok, txt = state_ok(ppm, ppm_ref, 0, 0)
    print(f"  phase 4 icons: {txt}", flush=True)
    check("phase 4: back to OFF/OFF -> icons OFF/OFF", ok)

    # ---- Phase 5: peer dead -> OFF, menu responsive ----
    stop_peer(peer)
    peer = None
    poke_menu(ser)
    time.sleep(12.0)  # 2+ poll periods
    ppm = dump_menu(mon, ser, "k5-nopeer")
    ok, txt = state_ok(ppm, ppm_ref, 0, 0)
    print(f"  phase 5 icons: {txt}", flush=True)
    check("phase 5: peer dead -> icons OFF/OFF (never stale ON)",
          ok)
    check("phase 5: menu still responds to keys (no poll wedge)",
          key(ser, "down", "UILV2: focus ", 32))
    release_to_shell(ser)
    # Probe, not a single shell_cmd: bytes sent during
    # an unowned-console window are drained through
    # translate_key ('m' -> EV_MENU) instead of
    # reaching sh's line editor -- a pre-existing
    # console-drain race (spontaneous re-grants in
    # the measurement runs came from the same cause).
    # Retry until the setup block actually appears.
    probe_ok = False
    for _ in range(5):
        pos = len(serial)
        ser.sendall(b"setup\n")
        if wait_from(pos, "  name: ", 15):
            probe_ok = True
            break
        time.sleep(1.0)
    check("phase 5: shell prompt back after release", probe_ok)
    t_grant = grant_menu_timed(ser)
    print(f"  grant->menu (peer dead): {t_grant:.1f}s", flush=True)
    check("phase 5: re-grant starts the shell menu (peer dead)",
          t_grant >= 0)
    check("menu appears fast with peer dead (< 25 s)",
          0 <= t_grant < 25)
    poke_menu(ser)
    time.sleep(8.0)
    ppm = dump_menu(mon, ser, "k5-nopeer-menu")
    ok, txt = state_ok(ppm, ppm_ref, 0, 0)
    print(f"  phase 5b icons: {txt}", flush=True)
    check("phase 5b: re-granted menu still OFF/OFF (peer dead)",
          ok)

    seg = serial.decode("utf-8", "replace")
    check("no kernel FAULT anywhere", "FAULT" not in seg)
    check("no uiapp FAIL anywhere", "uiapp FAIL" not in seg)
    check("no UILV2 flush failures", "UILV2: flush failures" not in seg)

    # Teardown: restore name + tz, disk unprovisioned.
    # PIN 246810 untouched throughout (never changed).
    release_to_shell(ser)
    shell_cmd(ser, "setup name qaon", "setup: name saved", 10)
    shell_cmd(ser, "setup tz +0700", "setup: tz saved", 10)
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(1.0)

    # Size evidence (objcopy-binary method).
    kbin = os.path.join(LOGDIR, "k5-kernel.bin")
    subprocess.run([tc + "/usr/bin/llvm-objcopy-18", "-O", "binary",
                    KERNEL, kbin], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ksize = os.path.getsize(kbin)
    print(f"  kernel binary={ksize} headroom={4194304 - ksize}",
          flush=True)
    check("kernel binary fits the 4 MiB boot partition",
          ksize < 4194304)
finally:
    try:
        with open(os.path.join(LOGDIR, "k5-serial.log"), "wb") as f:
            f.write(serial)
    except Exception:
        pass
    if peer is not None:
        stop_peer(peer)
    if qemu is not None:
        try:
            if qemu.poll() is None:
                mon.sendall(b"quit\n")
        except Exception:
            pass
        time.sleep(0.5)
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
    leftq = ""
    for _ in range(10):
        leftq = subprocess.run(["pgrep", "-x", "qemu-system-arm"],
                               capture_output=True, text=True).stdout.strip()
        if leftq == "":
            break
        time.sleep(0.5)
    check("no qemu-system-arm left running", leftq == "")
    # C6L-5 pattern: plain pgrep -f (this script's own
    # command line contains no "mock-c6.py").
    leftp = subprocess.run(["pgrep", "-f", "mock-c6.py"],
                           capture_output=True, text=True).stdout.strip()
    check("no mock-c6.py left running", leftp == "")

npass = sum(1 for _, c in results if c)
print(f"K5-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
