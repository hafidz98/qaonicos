#!/usr/bin/env python3
"""UI4-VERIFY: LVGL Monitor + Power screens (UI-4) + tz-clock follow-up.

The shell's Monitor placeholder becomes a real LVGL screen
(real SYS_STAT / bootstate / cfg-name / clock rows, 500 ms
refresh, BACK to menu) and the Power entry's old
hand-rolled scr_power wiring becomes a real LVGL screen
(SHUTDOWN / RESTART / SLEEP NOW / BACK, scr_power's
semantics: immediate action, no confirmation). The
follow-up item: device clocks honor cfg-tz (both
fmt_clock copies, WIB fallback on malformed values).

This script boots QEMU, drives the standard flow
(setup reset -> gui -> setup done), sets known state
via the shell (setup name/tz; the shared PIN 246810 is
NEVER changed -- UI-3's battery lesson), then checks:

  * the shell menu still renders and 6x down reaches
    Monitor, OK opens it ("UILV4: monitor", "UILV4:
    mon ..." rows: Device TestDev, Uptime, Memory,
    Threads, Net, Disk, CPU "-", Boot #, Time);
  * the Monitor dump is light (LVGL) with a dark
    title band, dark row bands, and a blue BACK
    button; BACK returns to the menu;
  * 7x down reaches Power, OK opens it ("UILV4:
    power") with four blue buttons; BACK key
    returns to the menu ("UILV4: back menu");
  * tz-clock (LVGL path): shell `time` + setup tz
    +0800 -> Monitor Time row matches the +0800
    clock (+-1 min); setup tz bogus -> Monitor Time
    row matches the WIB fallback; setup tz +0700
    restores (setup status reads back +0700);
  * tz-clock (old path): the Wi-Fi old screen's
    status-bar clock matches the +0700 clock by
    exact 5x7 glyph template match (font5x7.h);
  * Power actions on fresh boots: Shutdown ->
    "UILV4: power shutdown" + QEMU exits on its own
    (exit 0); Restart -> "UILV4: power restart" +
    SYS_RESET logged + new qaon> prompt; Sleep ->
    "UILV4: power sleep" + token release + a dump
    that differs from the Power screen;
  * no FAULT, no uiapp FAIL, no UILV2/UILV4 flush
    failures; teardown restores tz +0700 and name
    qaon, leaves the disk unprovisioned and no QEMU
    running; PIN 246810 untouched throughout;
  * size evidence: uiapp.bin size and objcopy-binary
    headroom are printed (method: llvm-objcopy-18 -O
    binary on the linked mach3.elf vs the 4,194,304 B
    boot partition, same as C6L-3/4/5/UI-1/UI-2/UI-3).

Pre-change shape: on the UI-3 kernel Monitor is
still the honest placeholder ("UILV2: placeholder
monitor", no UILV4 marker) and Power is the old
scr_power screen -- the script FAILs on every UILV4
check. Set KERNEL to that ELF to reproduce.

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first;
scratch lives under ~/workspace/logs-ui4 (NOT /tmp).
Full output is kept (no tail truncation): every
non-pass check name is printed by the script itself.
"""
import os
import re
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get("KERNEL", ROOT + "/mach3-port/build/mach3.elf")
TSCALE = float(os.environ.get("UI4_TSCALE", "1.0"))
LOGDIR = os.path.expanduser("~/workspace/logs-ui4")
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


SER = os.path.join(LOGDIR, "ui4-ser.sock")
MON = os.path.join(LOGDIR, "ui4-mon.sock")
for s in (SER, MON):
    try:
        os.unlink(s)
    except FileNotFoundError:
        pass

serial = b""


T0 = time.time()
mark_log = []


def start_reader(ser):
    def _reader():
        global serial
        while True:
            try:
                d = ser.recv(4096)
                if d:
                    serial += d
                    # Timestamped marker arrivals: adjudicates
                    # wait-expired-vs-device-slow disputes.
                    if (b"UILV2:" in d or b"UILV4:" in d or
                            b"key: injected" in d or
                            b"perintah tak dikenal" in d):
                        mark_log.append((round(time.time() - T0, 1),
                                         d[:200]))
            except socket.timeout:
                pass
            except OSError:
                return
    threading.Thread(target=_reader, daemon=True).start()


def wait_from(pos, needle, timeout=60):
    timeout = timeout * TSCALE
    t0 = time.time()
    nb = needle.encode()
    while time.time() - t0 < timeout:
        if nb in serial[pos:]:
            return True
        time.sleep(0.2)
    return nb in serial[pos:]


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
    # A previous QEMU (e.g. poweroff-exited) may leave
    # stale socket files behind; unlink first so conn()
    # never dials a dead socket.
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


def close_qemu(p, mon):
    try:
        mon.sendall(b"quit\n")
    except Exception:
        pass
    time.sleep(0.5)
    try:
        p.wait(timeout=10)
    except Exception:
        p.kill()
    time.sleep(0.5)


def find_after(pos, needle):
    return needle.encode() in serial[pos:]


def wait_shell_ready(ser, tries=3):
    """Probe that sh answers before a `time` read.

    'setup status' prints a '  tz: ...' line; seeing it proves
    sh processed a command after the last release, so the
    following `time` send is not racing shell state.
    (Note: face cannot steal console bytes -- sh takes
    console ownership at startup and never releases it --
    so a failed probe means sh itself is unresponsive, not
    a face drain.)
    """
    for _ in range(tries):
        pos = len(serial)
        ser.sendall(b"setup status\n")
        if wait_from(pos, "  tz: ", 5):
            return True
    return False


def shell_cmd(ser, cmd, wait_for, timeout=30):
    cb = cmd.encode()
    for _ in range(3):
        pos = len(serial)
        ser.sendall(cb + b"\n")
        if wait_for == "" or wait_from(pos, wait_for, timeout):
            return True
        seg = serial[pos:]
        if b"\n" + cb in seg or b"qaon> " + cb in seg:
            return False
    return False


def screendump(mon, name):
    ppm = os.path.join(LOGDIR, name + ".ppm")
    mon.sendall(f"screendump {ppm}\n".encode())
    time.sleep(1.0)
    try:
        mon.recv(4096)
    except Exception:
        pass
    return ppm if os.path.exists(ppm) else None


def band_stats(ppm, y0, y1):
    """(dark, blue) pixel counts in rows [y0, y1)."""
    from PIL import Image
    im = Image.open(ppm).convert("RGB")
    dark = blue = 0
    for yy in range(y0, y1):
        for xx in range(240):
            r, g, b = im.getpixel((xx, yy))
            if r + g + b < 300:
                dark += 1
            if b > 150 and b - r > 80:
                blue += 1
    return im, dark, blue


def white_pixels(ppm):
    from PIL import Image
    im = Image.open(ppm).convert("RGB")
    n = 0
    for yy in range(240):
        for xx in range(240):
            r, g, b = im.getpixel((xx, yy))
            if r > 240 and g > 240 and b > 240:
                n += 1
    return n


def frame_diff(a, b):
    n = 0
    for yy in range(240):
        for xx in range(240):
            if a.getpixel((xx, yy)) != b.getpixel((xx, yy)):
                n += 1
    return n


def key(ser, k, needle, timeout=20):
    """Send one key, verifying sh ran it.

    PL011 RX under cooperative scheduling can drop a byte
    ('key menu' -> 'ey menu' -> unknown command, seen in
    run 2); 'key: injected' proves sh parsed the full line,
    so a missing confirmation triggers one resend.
    """
    for _ in range(2):
        pos = len(serial)
        ser.sendall(f"key {k}\n".encode())
        if wait_from(pos, "key: injected", 5):
            break
    return wait_from(pos, needle, timeout)


def send_keys(ser, seq, delay=0.35):
    for k in seq:
        for _ in range(2):
            pos = len(serial)
            ser.sendall(f"key {k}\n".encode())
            if wait_from(pos, "key: injected", 5):
                break
        time.sleep(delay * TSCALE)


def grant_menu(ser):
    """Fresh token grant -> shell menu, with retries.

    A grant can be lost when face is not up yet on a fresh
    boot, and a menu may already run from a stolen-'m'
    grant (see wait_shell_ready): a focus marker after our
    send proves the menu is alive, which counts as granted.
    """
    for _ in range(3):
        pos = len(serial)
        ser.sendall(b"key menu\n")
        if not wait_from(pos, "key: injected", 5):
            # Byte-loss on the send ('ey menu'): resend once
            # before spending 20 s waiting for a grant that
            # can never arrive.
            pos = len(serial)
            ser.sendall(b"key menu\n")
            if not wait_from(pos, "key: injected", 5):
                continue
        if wait_from(pos, "UILV2: start", 20):
            return True
        if find_after(pos, "UILV2: focus"):
            return True
    return False


def setup_flow(ser):
    """setup reset -> gui -> setup done (finale)."""
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(0.6)
    ser.sendall(b"gui\n")
    if not wait_from(0, "face: setup mode, payload", 60):
        return False
    finale = False
    for _ in range(4):
        ser.sendall(b"setup done\n")
        if wait_from(len(serial) - 200, "provisioned -> finale", 10):
            finale = True
            break
    return finale


def goto_entry(ser, downs, marker, timeout=20):
    """From the shell menu (focus Qabot): walk down to
    an entry by focus markers, OK -> marker."""
    want = ("wifi", "ble", "llm", "passkey", "settings",
            "monitor", "power")
    for i in range(downs):
        if not key(ser, "down", "UILV2: focus " + want[i], 20):
            return False
    return key(ser, "ok", marker, timeout)


def old_clock_check(ser, mon, off_min, label):
    """Old-path clock proof: read shell time, open the
    Wi-Fi old screen, template-match its status-bar
    clock against the expected HH:MM for off_min
    (+-1 min rollover), then BACK out to the shell.
    Returns True on an exact glyph match."""
    t = read_unix_time(ser)
    if t is None:
        print(f"  {label}: shell time unreadable", flush=True)
        return False
    if not grant_menu(ser):
        return False
    if not goto_entry(ser, 1, "UILV2: oldscreen wifi", 20):
        return False
    time.sleep(2.0)
    ppm = screendump(mon, "ui4-wifi-clock-" + label)
    glyphs = load_font5x7()
    if ppm is None or glyphs is None:
        return False
    ok = False
    for (hh, mm) in sorted(expected_hhmm(t, off_min)):
        hhmm = f"{hh:02d}:{mm:02d}"
        m, e, x = match_old_clock(ppm, hhmm, glyphs)
        print(f"  {label} try {hhmm}: matched={m}/{e} extra={x}",
              flush=True)
        if m == e and x == 0:
            ok = True
            break
    # BACK out: the Wi-Fi screen eats its first
    # keypress to dismiss its no-peer note (UI-2
    # lesson), so a second BACK may be needed.
    if not key(ser, "back", "UILV2: start", 10):
        if not key(ser, "back", "UILV2: start", 20):
            return False
    if not key(ser, "back", "UILV2: release", 20):
        return False
    return ok


def read_unix_time(ser):
    """Shell `time` -> unix seconds, or None.

    Retries through wait_shell_ready so a transiently
    unresponsive shell does not poison the tz checks.
    """
    for _ in range(3):
        if not wait_shell_ready(ser):
            continue
        pos = len(serial)
        ser.sendall(b"time\n")
        if not wait_from(pos, "time=", 10):
            continue
        seg = serial[pos:].decode("utf-8", "replace")
        m = re.search(r"time=(\d+)", seg)
        if m:
            return int(m.group(1))
    return None


def expected_hhmm(unix, off_min):
    """(hh, mm) for unix + offset, plus the +1 min
    rollover candidate (execution gap tolerance)."""
    day = (unix % 86400 + 86400 + off_min * 60) % 86400
    hh, mm = day // 3600, (day // 60) % 60
    mm2 = mm + 1
    hh2 = hh
    if mm2 == 60:
        mm2 = 0
        hh2 = (hh + 1) % 24
    return {(hh, mm), (hh2, mm2)}


def mon_time_row(ser, pos=0):
    """Most recent 'UILV4: mon Time: HH:MM' at/after pos.

    Positional: searching the whole history returns a stale
    row from an earlier Monitor visit when the current
    visit's grant failed.
    """
    seg = serial[pos:].decode("utf-8", "replace")
    ms = re.findall(r"UILV4: mon Time: (\d\d):(\d\d)", seg)
    if not ms:
        return None
    return (int(ms[-1][0]), int(ms[-1][1]))


def load_font5x7():
    """Glyph columns for '0'-'9' and ':' from font5x7.h."""
    txt = open(ROOT + "/user/face/font5x7.h").read()
    glyphs = {}
    for ch in "0123456789:":
        m = re.search(r"\{\s*'" + re.escape(ch) + r"'\s*,\s*\{([^}]*)\}",
                      txt)
        if not m:
            return None
        glyphs[ch] = [int(x.strip(), 0)
                      for x in m.group(1).split(",") if x.strip()]
    return glyphs


def match_old_clock(ppm, hhmm, glyphs):
    """Exact 5x7 template match of the old status-bar
    clock at (6, 4), C_FG on the dark bar. Returns
    (matched, expected_on, extra_on)."""
    from PIL import Image
    im = Image.open(ppm).convert("RGB")
    expected = set()
    for k, ch in enumerate(hhmm):
        cols = glyphs[ch]
        for col in range(5):
            for row in range(7):
                if (cols[col] >> row) & 1:
                    expected.add((6 + k * 6 + col, 4 + row))
    matched = 0
    extra = 0
    for xx in range(6, 36):
        for yy in range(4, 11):
            r, g, b = im.getpixel((xx, yy))
            bright = (r + g + b) > 300
            if (xx, yy) in expected:
                if bright:
                    matched += 1
            elif bright:
                extra += 1
    return matched, len(expected), extra


qemu = None
ser = mon = None
try:
    qemu, ser, mon = launch_qemu()

    check("boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    check("setup flow reaches finale", setup_flow(ser))

    # Known state via the shell (independent channel).
    # PIN is NEVER touched (shared 246810, UI-3 lesson).
    check("shell sets device name",
          shell_cmd(ser, "setup name TestDev", "setup: name saved", 10))
    check("shell sets timezone +0700",
          shell_cmd(ser, "setup tz +0700", "setup: tz saved", 10))

    check("grant starts the shell menu", grant_menu(ser))
    time.sleep(2.0)
    ppm_menu = screendump(mon, "ui4-menu")
    check("menu screendump saved", ppm_menu is not None)
    if ppm_menu is not None:
        w = white_pixels(ppm_menu)
        print(f"  menu: white={w}", flush=True)
        check("menu renders light (LVGL shell)", w > 5000)

    # ---- Monitor ----
    check("6x down + OK opens Monitor",
          goto_entry(ser, 6, "UILV4: monitor", 20))
    time.sleep(1.0)
    seg = serial.decode("utf-8", "replace")
    for row in ("Device: TestDev", "Uptime:", "Memory:", "Threads:",
                "Net:", "Disk:", "CPU: -", "Boot: #", "Time:"):
        check(f"monitor row present: {row}",
              f"UILV4: mon {row}" in seg)
    ppm_mon = screendump(mon, "ui4-monitor")
    check("monitor screendump saved", ppm_mon is not None)
    if ppm_mon is not None and ppm_menu is not None:
        from PIL import Image
        im_mon, title_dark, _ = band_stats(ppm_mon, 24, 40)
        _, rows_dark, _ = band_stats(ppm_mon, 46, 190)
        _, _, back_blue = band_stats(ppm_mon, 208, 232)
        fd = frame_diff(Image.open(ppm_menu).convert("RGB"), im_mon)
        print(f"  monitor: title dark={title_dark} rows dark={rows_dark} "
              f"back blue={back_blue} frame diff={fd}", flush=True)
        check("monitor: title rendered (dark)", title_dark > 20)
        check("monitor: rows rendered (dark)", rows_dark > 100)
        check("monitor: BACK button rendered (blue)", back_blue > 1000)
        check("monitor: differs from the menu", fd > 1000)
    # BACK button -> menu (focus returns to Qabot).
    check("Monitor BACK button returns to the menu",
          key(ser, "ok", "UILV2: focus qabot", 20))

    # ---- Power screen ----
    check("7x down + OK opens Power",
          goto_entry(ser, 7, "UILV4: power", 20))
    time.sleep(1.0)
    ppm_pwr = screendump(mon, "ui4-power")
    check("power screendump saved", ppm_pwr is not None)
    if ppm_pwr is not None:
        for i, (y0, y1) in enumerate(((48, 72), (74, 98),
                                      (100, 124), (126, 150))):
            _, _, bb = band_stats(ppm_pwr, y0, y1)
            print(f"  power button {i} blue={bb}", flush=True)
            check(f"power: button {i} rendered (blue)", bb > 1000)
    # BACK key -> menu.
    check("Power BACK key returns to the menu",
          key(ser, "back", "UILV4: back menu", 20))

    # ---- tz-clock, LVGL path ----
    # Release to the shell, then drive tz from there.
    check("BACK at menu root releases",
          key(ser, "back", "UILV2: release", 20))
    t0 = read_unix_time(ser)
    check("shell time readable", t0 is not None)
    check("shell sets timezone +0800",
          shell_cmd(ser, "setup tz +0800", "setup: tz saved", 10))
    time.sleep(6.0)  # tz cache TTL (shell path writes around it)
    check("grant starts the shell menu (tz +0800)", grant_menu(ser))
    mon_pos = len(serial)
    check("6x down + OK opens Monitor (tz +0800)",
          goto_entry(ser, 6, "UILV4: monitor", 20))
    time.sleep(1.0)
    got = mon_time_row(ser, mon_pos)
    exp = expected_hhmm(t0, 480) if t0 else set()
    print(f"  tz +0800: monitor time={got} expected in {sorted(exp)}",
          flush=True)
    check("Monitor clock follows +0800", got in exp)
    # Old path at +0800: discriminating -- the UI-3
    # clock hardcodes WIB, so this FAILs pre-change.
    key(ser, "ok", "UILV2: focus qabot", 20)  # Monitor BACK -> menu
    key(ser, "back", "UILV2: release", 20)
    check("old status-bar clock follows +0800 (glyph-exact)",
          old_clock_check(ser, mon, 480, "old+0800"))
    # Malformed tz -> WIB fallback. Note: we are
    # already at the shell here -- old_clock_check
    # backs out to a release itself -- so no
    # Monitor-BACK / menu-release dance first.
    t1 = read_unix_time(ser)
    check("shell sets malformed timezone",
          shell_cmd(ser, "setup tz bogus", "setup: tz saved", 10))
    time.sleep(6.0)
    check("grant starts the shell menu (bogus tz)", grant_menu(ser))
    mon_pos = len(serial)
    check("6x down + OK opens Monitor (bogus tz)",
          goto_entry(ser, 6, "UILV4: monitor", 20))
    time.sleep(1.0)
    got = mon_time_row(ser, mon_pos)
    exp = expected_hhmm(t1, 420) if t1 else set()
    print(f"  tz bogus: monitor time={got} expected in {sorted(exp)}",
          flush=True)
    check("Monitor clock falls back to WIB on malformed tz",
          got in exp)
    # Restore +0700 (independent read-back via setup status).
    key(ser, "ok", "UILV2: focus qabot", 20)  # Monitor BACK -> menu
    key(ser, "back", "UILV2: release", 20)
    check("shell restores timezone +0700",
          shell_cmd(ser, "setup tz +0700", "setup: tz saved", 10))
    check("setup status reads back +0700",
          shell_cmd(ser, "setup", "tz: +0700", 10))
    time.sleep(6.0)

    # ---- tz-clock, old path at +0700 (after restore) ----
    check("old status-bar clock matches +0700 (glyph-exact)",
          old_clock_check(ser, mon, 420, "old+0700"))

    # ---- Power actions, one fresh boot each ----
    # Shutdown.
    close_qemu(qemu, mon)
    qemu, ser, mon = launch_qemu()
    check("shutdown boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    # No setup dance here: this relaunch lands on a
    # provisioned disk, where setup_flow cannot reach
    # finale (face picks setup-vs-normal at startup).
    # Dev mode boots shell-first: daemons launch only on
    # `gui` (run 2 proved a grant without gui can never
    # arrive -- no face exists to grant it).
    check("shutdown boot dashboard starts",
          shell_cmd(ser, "gui", "face: Qabot server starting", 60))
    check("shutdown grant starts the shell menu", grant_menu(ser))
    check("shutdown: 7x down + OK opens Power",
          goto_entry(ser, 7, "UILV4: power", 20))
    # focus SHUTDOWN -> immediate poweroff
    check("Shutdown action prints the marker",
          key(ser, "ok", "UILV4: power shutdown", 20))
    try:
        qemu.wait(timeout=30)
        exited = True
    except Exception:
        exited = False
    check("Shutdown: QEMU exits on its own", exited)
    check("Shutdown: QEMU exit code 0", exited and qemu.poll() == 0)

    # Restart. Defensive close first: if the Shutdown
    # QEMU is still alive (its action never ran), a
    # bare relaunch collides on hostfwd 18080 and the
    # new QEMU dies before creating its sockets (run 2
    # crash).
    close_qemu(qemu, mon)
    qemu, ser, mon = launch_qemu()
    check("restart boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    # No setup dance here: this relaunch lands on a
    # provisioned disk, where setup_flow cannot reach
    # finale (face picks setup-vs-normal at startup).
    # The menu grant works regardless of provisioning.
    check("restart boot dashboard starts",
          shell_cmd(ser, "gui", "face: Qabot server starting", 60))
    check("restart grant starts the shell menu", grant_menu(ser))
    check("restart: 7x down + OK opens Power",
          goto_entry(ser, 7, "UILV4: power", 20))
    check("restart: 1x down focuses RESTART",
          key(ser, "down", "UILV4: focus restart", 20))
    check("Restart action prints the marker",
          key(ser, "ok", "UILV4: power restart", 20))
    pos = len(serial)
    check("Restart: kernel logged SYS_RESET",
          wait_from(pos, "sys: SYS_RESET (software request)", 30))
    check("Restart: new boot reaches qaon> prompt",
          wait_from(pos, "qaon>", 90))

    # Sleep.
    close_qemu(qemu, mon)
    qemu, ser, mon = launch_qemu()
    check("sleep boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    # No setup dance here: this relaunch lands on a
    # provisioned disk, where setup_flow cannot reach
    # finale (face picks setup-vs-normal at startup).
    # The menu grant works regardless of provisioning.
    check("sleep boot dashboard starts",
          shell_cmd(ser, "gui", "face: Qabot server starting", 60))
    check("sleep grant starts the shell menu", grant_menu(ser))
    check("sleep: 7x down + OK opens Power",
          goto_entry(ser, 7, "UILV4: power", 20))
    send_keys(ser, ["down", "down"], 0.8)
    check("Sleep action prints the marker",
          key(ser, "ok", "UILV4: power sleep", 20))
    pos = len(serial)
    check("Sleep: token released",
          wait_from(pos, "UILV2: release", 20))
    time.sleep(1.0)
    ppm_sleep = screendump(mon, "ui4-sleep")
    check("sleep screendump saved", ppm_sleep is not None)
    if ppm_sleep is not None and ppm_pwr is not None:
        from PIL import Image
        fd = frame_diff(Image.open(ppm_pwr).convert("RGB"),
                        Image.open(ppm_sleep).convert("RGB"))
        print(f"  sleep: frame diff vs power screen={fd}", flush=True)
        check("sleep: display differs from the Power screen", fd > 1000)

    seg = serial.decode("utf-8", "replace")
    check("no kernel FAULT anywhere", "FAULT" not in seg)
    check("no uiapp FAIL anywhere", "uiapp FAIL" not in seg)
    check("no UILV2 flush failures", "UILV2: flush failures" not in seg)
    check("no UILV4 flush failures", "UILV4: flush failures" not in seg)

    # Teardown: restore name + tz, disk unprovisioned.
    # (Best effort: the Sleep boot's shell may still be
    # responsive after release; a fresh prompt is not
    # required for the verdict above.)
    shell_cmd(ser, "setup name qaon", "setup: name saved", 10)
    shell_cmd(ser, "setup tz +0700", "setup: tz saved", 10)
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(1.0)

    # Size evidence (objcopy-binary method).
    ubin = os.path.join(ROOT, "mach3-port/build/uiapp.bin")
    kbin = os.path.join(LOGDIR, "ui4-kernel.bin")
    subprocess.run([tc + "/usr/bin/llvm-objcopy-18", "-O", "binary",
                    KERNEL, kbin], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    usize = os.path.getsize(ubin) if os.path.exists(ubin) else -1
    ksize = os.path.getsize(kbin)
    print(f"  uiapp.bin={usize} kernel binary={ksize} "
          f"headroom={4194304 - ksize}", flush=True)
    check("kernel binary fits the 4 MiB boot partition",
          ksize < 4194304)
finally:
    try:
        with open(os.path.join(LOGDIR, "ui4-serial.log"), "wb") as f:
            f.write(serial)
        with open(os.path.join(LOGDIR, "ui4-marks.log"), "w") as f:
            for (t, d) in mark_log:
                f.write(f"{t} {d!r}\n")
    except Exception:
        pass
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
    # A kill takes a moment to reap: poll before the
    # pgrep verdict, or a dying QEMU reads as leftover.
    leftq = ""
    for _ in range(10):
        leftq = subprocess.run(["pgrep", "-x", "qemu-system-arm"],
                               capture_output=True, text=True).stdout.strip()
        if leftq == "":
            break
        time.sleep(0.5)
    check("no qemu-system-arm left running", leftq == "")

npass = sum(1 for _, c in results if c)
print(f"UI4-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
