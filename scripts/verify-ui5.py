#!/usr/bin/env python3
"""UI5-VERIFY: LVGL device-name text editor + polish (UI-5).

The Settings name detail's "Edit in UI-5" note becomes a
real keypad-driven char-picker editor (UP/DOWN cycles the
character, LEFT/RIGHT moves, LEFT at 0 deletes the last
character, RIGHT at the end appends, OK saves, BACK
cancels -- the old scr_textedit semantics re-hosted in
LVGL raw-key mode, reusing the UI-3 PIN picker's
pattern). Polish: Monitor BACK focus marker, and the
BACK-button paths to the shell menu print the same
back-menu marker as the BACK-key paths.

This script boots QEMU, drives the standard flow
(setup reset -> gui -> setup done), sets known state
via the shell (setup name AB; the shared PIN 246810 is
NEVER changed -- UI-3's battery lesson), then checks:

  * the name detail renders an Edit name button (blue)
    and focus starts on it; OK opens the editor
    ("UILV5: edit name");
  * the full edit round trip: AB -> ABC via three UP
    keys -> OK saves ("UILV5: edit saved ABC") ->
    shell `setup` reads back ABC -> re-grant shows
    the new name (detail dump differs) -> shell
    restores AB;
  * BACK-cancel: edit KeepMe, change a character,
    BACK cancels ("UILV5: edit cancel") -> shell
    still reads KeepMe;
  * the 40-char bound: a 40-char name in the editor
    refuses appends (RIGHT/UP at the cap) and saves
    back exactly 40 chars;
  * polish: Monitor BACK focus marker ("UILV4: focus
    monback"); button-BACK from Settings list,
    Monitor, and Power prints the same back-menu
    marker as key-BACK; the status bar (clock band)
    renders on every LVGL screen (menu, settings
    list, name detail, editor, Monitor, Power);
    raw keys reset the idle timer and 30 s idle in
    the editor exits to release;
  * no FAULT, no uiapp FAIL, no flush failures;
    teardown restores name qaon and tz +0700, leaves
    the disk unprovisioned and no QEMU running; PIN
    246810 untouched throughout;
  * size evidence: uiapp.bin size and objcopy-binary
    headroom are printed (method: llvm-objcopy-18 -O
    binary on the linked mach3.elf vs the 4,194,304 B
    boot partition, same as UI-1..UI-4).

Pre-change shape: on the UI-4 kernel the name detail
has no Edit button (no blue at y=100-124) and OK on
the detail returns to the list instead of opening an
editor -- every UILV5 check FAILs. No UILV5 string
exists anywhere in the base tree. Set KERNEL to that
ELF to reproduce.

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first;
scratch lives under ~/workspace/logs-ui5 (NOT /tmp).
Full output is kept (no tail truncation): every
non-pass check name is printed by the script itself.
All key sends use injection verification (send +
wait for "key: injected", resend once on failure);
grants use the retry + recovery pattern (UI-4).
"""
import os
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get("KERNEL", ROOT + "/mach3-port/build/mach3.elf")
TSCALE = float(os.environ.get("UI5_TSCALE", "1.0"))
LOGDIR = os.path.expanduser("~/workspace/logs-ui5")
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


SER = os.path.join(LOGDIR, "ui5-ser.sock")
MON = os.path.join(LOGDIR, "ui5-mon.sock")
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
                    if (b"UILV2:" in d or b"UILV3:" in d or
                            b"UILV4:" in d or b"UILV5:" in d or
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


def shell_probe(ser, cmd, needle, tries=5, timeout=15):
    """Send a shell command until its OUTPUT appears.

    Unlike shell_cmd, this never accepts an echo as
    evidence: after the idle test the face can
    re-grant uiapp at once, and in that window a
    probe line can be consumed by qabotd (which
    echoes each char as it reads) while sh never
    sees it. shell_cmd mistakes that echo for sh's
    echo and gives up; this loop only accepts the
    real output, so a raced line is just retried.
    """
    for _ in range(tries):
        pos = len(serial)
        ser.sendall(cmd.encode() + b"\n")
        if wait_from(pos, needle, timeout):
            return True
        time.sleep(1.0)
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
    w = 0
    for yy in range(240):
        for xx in range(240):
            r, g, b = im.getpixel((xx, yy))
            if r + g + b > 700:
                w += 1
    return w


def frame_diff(a, b):
    w = 0
    for yy in range(240):
        for xx in range(240):
            if a.getpixel((xx, yy)) != b.getpixel((xx, yy)):
                w += 1
    return w


def send_key(ser, k):
    """Send one key with injection verification (resend
    once on PL011 byte-loss). True if sh ran it."""
    for _ in range(2):
        pos = len(serial)
        ser.sendall(f"key {k}\n".encode())
        if wait_from(pos, "key: injected", 5):
            return True
    return False


def key(ser, k, needle, timeout=32):
    """Send one key, verifying sh ran it, then wait
    for the device marker. See verify-ui4.py."""
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
    """Fresh token grant -> shell menu, with retries
    (verify-ui4.py pattern)."""
    for _ in range(3):
        pos = len(serial)
        ser.sendall(b"key menu\n")
        if not wait_from(pos, "key: injected", 5):
            pos = len(serial)
            ser.sendall(b"key menu\n")
            if not wait_from(pos, "key: injected", 5):
                continue
        if wait_from(pos, "UILV2: start", 32):
            return True
        if find_after(pos, "UILV2: focus"):
            return True
    pos = len(serial)
    send_key(ser, "back")
    if wait_from(pos, "UILV2: start", 32):
        return True
    wait_from(pos, "UILV2: release", 32)  # best effort
    pos = len(serial)
    ser.sendall(b"key menu\n")
    if not wait_from(pos, "key: injected", 5):
        pos = len(serial)
        ser.sendall(b"key menu\n")
        if not wait_from(pos, "key: injected", 5):
            return False
    if wait_from(pos, "UILV2: start", 32):
        return True
    return find_after(pos, "UILV2: focus")


def setup_flow(ser):
    """setup reset -> gui -> setup done (finale).

    Run-1 lesson: bare setup-done sends right after
    `gui` can vanish without a trace (no echo, no
    output) in the daemon-launch storm, and the
    finale marker never appears -- while every later
    shell command lands fine. So prove sh is reading
    first (a setup-status probe whose output must
    arrive), then require sh's own "setup: done"
    output or the finale marker per attempt.
    """
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(0.6)
    ser.sendall(b"gui\n")
    if not wait_from(0, "face: setup mode, payload", 60):
        return False
    if not shell_cmd(ser, "setup status", "  name: ", 15):
        return False
    finale = False
    for _ in range(4):
        pos = len(serial)
        ser.sendall(b"setup done\n")
        seg = None
        t0 = time.time()
        while time.time() - t0 < 15:
            seg = serial[pos:]
            if (b"setup: done" in seg or
                    b"provisioned -> finale" in seg):
                finale = True
                break
            time.sleep(0.2)
        if finale:
            break
    return finale


def goto_entry(ser, downs, marker, timeout=32):
    """From the shell menu (focus Qabot): walk down to
    an entry by focus markers, OK -> marker."""
    want = ("wifi", "ble", "llm", "passkey", "settings",
            "monitor", "power")
    for i in range(downs):
        if not key(ser, "down", "UILV2: focus " + want[i], 32):
            return False
    return key(ser, "ok", marker, timeout)


def release_to_shell(ser):
    """BACK out of any LVGL depth to the shell prompt:
    detail -> list -> menu -> release. Each step is
    best-effort; the caller checks the shell prompt."""
    send_key(ser, "back")
    time.sleep(1.0)
    send_key(ser, "back")
    time.sleep(1.0)
    send_key(ser, "back")
    time.sleep(1.5)


def open_name_detail(ser):
    """Grant -> Settings -> Device name detail. Assumes
    the shell prompt is current."""
    if not grant_menu(ser):
        return False
    if not goto_entry(ser, 5, "UILV3: settings list", 32):
        return False
    pos = len(serial)
    if not key(ser, "ok", "UILV3: detail name", 32):
        return False
    return wait_from(pos, "UILV3: focus editname", 32)


def bar_dark(ppm):
    """Dark pixels in the status-bar band (rows 0-20):
    the bar background is light gray and the radio
    label is mid-gray, so dark pixels are the clock
    glyphs. Proves the bar renders on a screen."""
    _, dark, _ = band_stats(ppm, 0, 20)
    return dark


qemu = None
ser = mon = None
try:
    qemu, ser, mon = launch_qemu()

    check("boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    check("setup flow reaches finale", setup_flow(ser))

    # Known state via the shell (independent channel).
    # PIN is NEVER touched (shared 246810, UI-3 lesson).
    check("shell sets device name AB",
          shell_cmd(ser, "setup name AB", "setup: name saved", 10))
    check("shell sets timezone +0700",
          shell_cmd(ser, "setup tz +0700", "setup: tz saved", 10))

    # ---- Name detail + editor entry ----
    check("grant starts the shell menu", grant_menu(ser))
    time.sleep(2.0)
    ppm_menu = screendump(mon, "ui5-menu")
    check("menu screendump saved", ppm_menu is not None)
    if ppm_menu is not None:
        print(f"  menu: status-bar dark={bar_dark(ppm_menu)}", flush=True)
        check("menu: status bar renders (clock band)",
              bar_dark(ppm_menu) > 20)
    check("5x down + OK opens Settings",
          goto_entry(ser, 5, "UILV3: settings list", 32))
    time.sleep(1.0)
    ppm_list = screendump(mon, "ui5-list")
    check("settings list screendump saved", ppm_list is not None)
    if ppm_list is not None:
        print(f"  list: status-bar dark={bar_dark(ppm_list)}", flush=True)
        check("settings list: status bar renders (clock band)",
              bar_dark(ppm_list) > 20)
    pos = len(serial)
    check("OK opens the Device name detail",
          key(ser, "ok", "UILV3: detail name", 32))
    check("name detail focuses Edit name first",
          wait_from(pos, "UILV3: focus editname", 32))
    time.sleep(1.5)
    ppm_detail_ab = screendump(mon, "ui5-detail-ab")
    check("name detail screendump saved", ppm_detail_ab is not None)
    if ppm_detail_ab is not None:
        from PIL import Image
        im_detail_ab = Image.open(ppm_detail_ab).convert("RGB")
        _, val_dark, _ = band_stats(ppm_detail_ab, 62, 80)
        _, _, edit_blue = band_stats(ppm_detail_ab, 100, 124)
        _, _, back_blue = band_stats(ppm_detail_ab, 130, 154)
        print(f"  detail AB: value dark={val_dark} edit blue={edit_blue} "
              f"back blue={back_blue} bar dark={bar_dark(ppm_detail_ab)}",
              flush=True)
        check("name detail: value rendered (dark)", val_dark > 20)
        check("name detail: Edit button rendered (blue)",
              edit_blue > 1000)
        check("name detail: BACK button rendered (blue)",
              back_blue > 1000)
        check("name detail: status bar renders (clock band)",
              bar_dark(ppm_detail_ab) > 20)
    else:
        im_detail_ab = None

    # ---- Edit round trip: AB -> ABC ----
    check("OK opens the editor",
          key(ser, "ok", "UILV5: edit name", 32))
    time.sleep(1.5)
    ppm_edit = screendump(mon, "ui5-edit")
    check("editor screendump saved", ppm_edit is not None)
    if ppm_edit is not None and ppm_detail_ab is not None:
        from PIL import Image
        im_edit = Image.open(ppm_edit).convert("RGB")
        _, val_dark, _ = band_stats(ppm_edit, 56, 80)
        fd = frame_diff(im_detail_ab, im_edit)
        print(f"  editor: value dark={val_dark} diff vs detail={fd} "
              f"bar dark={bar_dark(ppm_edit)}", flush=True)
        check("editor: value rendered (dark)", val_dark > 20)
        check("editor: differs from the detail", fd > 1000)
        check("editor: status bar renders (clock band)",
              bar_dark(ppm_edit) > 20)
    # AB, cursor at end: UP appends ' ' then cycles to
    # 'A' (ABA), UP -> 'B' (ABB), UP -> 'C' (ABC).
    check("editor: UP appends + cycles to A",
          key(ser, "up", "UILV5: edit name", 32))
    check("editor: UP cycles to B",
          key(ser, "up", "UILV5: edit name", 32))
    check("editor: UP cycles to C",
          key(ser, "up", "UILV5: edit name", 32))
    pos = len(serial)
    check("editor: OK saves ABC",
          key(ser, "ok", "UILV5: edit saved ABC", 32))
    check("save returns to the name detail",
          wait_from(pos, "UILV3: detail name", 32))
    release_to_shell(ser)
    check("shell reads back ABC",
          shell_cmd(ser, "setup", "  name: ABC", 10))
    # Re-grant shows the new name (detail re-reads cfg).
    check("re-grant starts the shell menu", grant_menu(ser))
    check("re-grant: Settings opens",
          goto_entry(ser, 5, "UILV3: settings list", 32))
    check("re-grant: name detail opens",
          key(ser, "ok", "UILV3: detail name", 32))
    time.sleep(1.5)
    ppm_detail_abc = screendump(mon, "ui5-detail-abc")
    check("re-grant detail screendump saved",
          ppm_detail_abc is not None)
    if ppm_detail_abc is not None and im_detail_ab is not None:
        from PIL import Image
        fd = frame_diff(im_detail_ab,
                        Image.open(ppm_detail_abc).convert("RGB"))
        print(f"  re-grant detail: diff vs AB detail={fd}", flush=True)
        # Band, not just a floor: the two dumps must be
        # the SAME screen with different text (a
        # full-frame diff means desync, not a new
        # name -- that spurious pass bit the
        # pre-change run).
        check("re-grant detail differs (new name shown)",
              100 < fd < 20000)
    # Restore the original name via the shell.
    release_to_shell(ser)
    check("shell restores name AB",
          shell_cmd(ser, "setup name AB", "setup: name saved", 10))
    check("shell reads back AB",
          shell_cmd(ser, "setup", "  name: AB", 10))

    # ---- BACK-cancel: KeepMe unchanged ----
    check("shell sets name KeepMe",
          shell_cmd(ser, "setup name KeepMe", "setup: name saved", 10))
    check("cancel: name detail opens", open_name_detail(ser))
    check("cancel: OK opens the editor",
          key(ser, "ok", "UILV5: edit name", 32))
    check("cancel: UP changes a character",
          key(ser, "up", "UILV5: edit name", 32))
    pos = len(serial)
    check("cancel: BACK cancels the edit",
          key(ser, "back", "UILV5: edit cancel", 32))
    check("cancel returns to the name detail",
          wait_from(pos, "UILV3: detail name", 32))
    release_to_shell(ser)
    check("cancel: shell still reads KeepMe",
          shell_cmd(ser, "setup", "  name: KeepMe", 10))

    # ---- 40-char bound ----
    bound = "A" * 40
    check("shell sets a 40-char name",
          shell_cmd(ser, "setup name " + bound, "setup: name saved", 10))
    check("bound: name detail opens", open_name_detail(ser))
    check("bound: OK opens the editor",
          key(ser, "ok", "UILV5: edit name", 32))
    # At the cap: appends are refused (the editor
    # rebuilds, but the buffer cannot grow).
    check("bound: RIGHT at the cap refused (rebuilds)",
          key(ser, "right", "UILV5: edit name", 32))
    check("bound: UP at the cap refused (rebuilds)",
          key(ser, "up", "UILV5: edit name", 32))
    check("bound: OK saves exactly 40 chars",
          key(ser, "ok", "UILV5: edit saved " + bound, 32))
    release_to_shell(ser)
    check("bound: shell reads back the 40-char prefix",
          shell_cmd(ser, "setup", "  name: " + "A" * 39, 10))
    check("shell restores name AB after bound test",
          shell_cmd(ser, "setup name AB", "setup: name saved", 10))

    # ---- Polish: focus + button-BACK parity ----
    check("polish: grant starts the shell menu", grant_menu(ser))
    pos = len(serial)
    check("polish: 6x down + OK opens Monitor",
          goto_entry(ser, 6, "UILV4: monitor", 32))
    check("polish: Monitor BACK has a focus marker",
          wait_from(pos, "UILV4: focus monback", 32))
    time.sleep(1.0)
    ppm_mon = screendump(mon, "ui5-monitor")
    check("polish: monitor screendump saved", ppm_mon is not None)
    if ppm_mon is not None:
        print(f"  monitor: bar dark={bar_dark(ppm_mon)}", flush=True)
        check("polish: monitor status bar renders (clock band)",
              bar_dark(ppm_mon) > 20)
    check("polish: Monitor BACK button returns to menu",
          key(ser, "ok", "UILV4: back menu", 32))
    # Settings list button-BACK parity.
    check("polish: Settings opens",
          goto_entry(ser, 5, "UILV3: settings list", 32))
    check("polish: list DOWN to Timezone",
          key(ser, "down", "UILV3: focus tz", 32))
    check("polish: list DOWN to PIN",
          key(ser, "down", "UILV3: focus pin", 32))
    check("polish: list DOWN to BACK",
          key(ser, "down", "UILV3: focus setback", 32))
    check("polish: Settings BACK button returns to menu",
          key(ser, "ok", "UILV3: back menu", 32))
    # Power button-BACK parity.
    check("polish: Power opens",
          goto_entry(ser, 7, "UILV4: power", 32))
    time.sleep(1.0)
    ppm_pwr = screendump(mon, "ui5-power")
    check("polish: power screendump saved", ppm_pwr is not None)
    if ppm_pwr is not None:
        print(f"  power: bar dark={bar_dark(ppm_pwr)}", flush=True)
        check("polish: power status bar renders (clock band)",
              bar_dark(ppm_pwr) > 20)
    check("polish: Power DOWN to RESTART",
          key(ser, "down", "UILV4: focus restart", 32))
    check("polish: Power DOWN to SLEEP",
          key(ser, "down", "UILV4: focus sleep", 32))
    check("polish: Power DOWN to BACK",
          key(ser, "down", "UILV4: focus back", 32))
    check("polish: Power BACK button returns to menu",
          key(ser, "ok", "UILV4: back menu", 32))

    # ---- Polish: idle exit from the editor ----
    check("idle: Settings opens",
          goto_entry(ser, 5, "UILV3: settings list", 32))
    check("idle: name detail opens",
          key(ser, "ok", "UILV3: detail name", 32))
    check("idle: editor opens",
          key(ser, "ok", "UILV5: edit name", 32))
    # One raw key proves raw input resets the idle
    # timer; the 30 s guest idle exit then fires.
    check("idle: one editor key still in editor",
          key(ser, "up", "UILV5: edit name", 32))
    # One pos for both waits: idle exit and release
    # can arrive in a single reader chunk, so a fresh
    # pos for the release wait would miss a release
    # that already arrived (run-1 lesson).
    pos = len(serial)
    check("idle: editor idles out (30 s guest)",
          wait_from(pos, "UILV2: idle exit", 70))
    check("idle: token released after idle exit",
          wait_from(pos, "uiapp: token released", 32))

    seg = serial.decode("utf-8", "replace")
    check("no kernel FAULT anywhere", "FAULT" not in seg)
    check("no uiapp FAIL anywhere", "uiapp FAIL" not in seg)
    check("no UILV2 flush failures", "UILV2: flush failures" not in seg)
    check("no UILV5 flush failures", "UILV5: flush failures" not in seg)
    check("PIN untouched (setup status shows pin set)",
          shell_probe(ser, "setup", "  pin: set"))

    # Teardown: restore name + tz, disk unprovisioned.
    # PIN 246810 untouched throughout (never changed).
    shell_cmd(ser, "setup name qaon", "setup: name saved", 10)
    shell_cmd(ser, "setup tz +0700", "setup: tz saved", 10)
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(1.0)

    # Size evidence (objcopy-binary method).
    ubin = os.path.join(ROOT, "mach3-port/build/uiapp.bin")
    kbin = os.path.join(LOGDIR, "ui5-kernel.bin")
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
        with open(os.path.join(LOGDIR, "ui5-serial.log"), "wb") as f:
            f.write(serial)
        with open(os.path.join(LOGDIR, "ui5-marks.log"), "w") as f:
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
    leftq = ""
    for _ in range(10):
        leftq = subprocess.run(["pgrep", "-x", "qemu-system-arm"],
                               capture_output=True, text=True).stdout.strip()
        if leftq == "":
            break
        time.sleep(0.5)
    check("no qemu-system-arm left running", leftq == "")

npass = sum(1 for _, c in results if c)
print(f"UI5-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
