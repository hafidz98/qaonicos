#!/usr/bin/env python3
"""UI5-VERIFY: name detail web-note + status-bar icons (UI-7
revision of the UI-5 script).

UI-7 decision: device-name editing is WEB CONFIG ONLY.
The UI-5 char-picker editor is removed: the Settings
name detail shows the value + a "Edit via web settings"
note, focus starts on BACK, and OK on BACK returns to
the list. No UILV5 marker may appear anywhere. The
web-config write path is exercised through the shell
`setup name` command (the same littlefs cfg-name the
web page writes); the device side only has to display
it. Polish (UI-5) is kept: Monitor BACK focus marker,
and the BACK-button paths print the same back-menu
marker as the BACK-key paths.

Status-bar icons (UI-8): the shell's "WiFi BT [ ]"
text label is replaced by icon-font labels (Material
Symbols subset, 18 px) -- wifi_off + bluetooth_disabled
+ battery outline-only -- right-aligned in the bar.
Icon checks count mid-gray pixels per label box and
require gray above/below the old 11 px mask band
(rows <= 5 and >= 18), which the UI-7 masks cannot
reach; the clock-band dark check still proves the
clock renders. Glyph shapes are the visual proof,
read by the coordinator from the dumps.

This script boots QEMU, drives the standard flow
(setup reset -> gui -> setup done), sets known state
via the shell (setup name AB; the shared PIN 246810 is
NEVER changed -- UI-3's battery lesson), then checks:

  * the name detail renders the value + web note
    (dark) and focus starts on BACK; OK on BACK
    returns to the settings list; no UILV5 marker;
  * the rename round trip through the shell: AB ->
    ABC via `setup name` -> re-grant shows the new
    name (detail dump differs) -> shell restores AB;
  * BACK-button parity: BACK from the detail returns
    to the list and the name is unchanged;
  * the 40-char bound: a 40-char name set via the
    shell renders in the detail and reads back as
    the 39-char legacy display prefix;
  * polish: Monitor BACK focus marker ("UILV4: focus
    monback"); button-BACK from Settings list,
    Monitor, and Power prints the same back-menu
    marker as key-BACK; the status bar (clock band
    + icons) renders on every LVGL screen (menu,
    settings list, name detail, Monitor, Power);
    30 s idle in the detail exits to release;
  * no FAULT, no uiapp FAIL, no flush failures, no
    UILV5 markers; teardown restores name qaon and
    tz +0700, leaves the disk unprovisioned and no
    QEMU running; PIN 246810 untouched throughout;
  * size evidence: uiapp.bin size and objcopy-binary
    headroom are printed (method: llvm-objcopy-18 -O
    binary on the linked mach3.elf vs the 4,194,304 B
    boot partition, same as UI-1..UI-6).

Pre-change shape: on the UI-6 kernel the name detail
has an Edit button (blue at y=100-124), OK opens the
editor ("UILV5: edit name"), and no web note renders
-- the note, BACK-focus, no-editor, and no-UILV5
checks FAIL. Set KERNEL to that ELF to reproduce.

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


def shell_cmd_chunked(ser, cmd, wait_for, timeout=30, chunk=8, delay=0.3):
    """Send a long shell line in small chunks with pauses.

    A long single-send burst loses chars in the PL011
    RX FIFO when the guest drains slowly (UI-6/UI-7
    bound flake: 29 of 40 A's stored, three times).
    Chunking keeps the line identical -- only the
    delivery pacing changes -- and the caller still
    validates the full stored value, so a bad
    delivery fails loudly instead of being greened.
    """
    cb = cmd.encode()
    for _ in range(2):
        pos = len(serial)
        for i in range(0, len(cb), chunk):
            ser.sendall(cb[i:i + chunk])
            time.sleep(delay * TSCALE)
        ser.sendall(b"\n")
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
    the shell prompt is current. UI-7: focus starts
    on BACK (no editor)."""
    if not grant_menu(ser):
        return False
    if not goto_entry(ser, 5, "UILV3: settings list", 32):
        return False
    pos = len(serial)
    if not key(ser, "ok", "UILV3: detail name", 32):
        return False
    return wait_from(pos, "UILV3: focus nameback", 32)


def bar_dark(ppm):
    """Dark pixels in the status-bar band (rows 0-20):
    the bar background is light gray and the icons
    are mid-gray, so dark pixels are the clock
    glyphs. Proves the bar renders on a screen."""
    _, dark, _ = band_stats(ppm, 0, 20)
    return dark


def _is_icon_gray(px):
    r, g, b = px[0], px[1], px[2]
    return 100 < r < 200 and max(r, g, b) - min(r, g, b) < 30


def icons_ok(ppm):
    """Status-bar icons (UI-8): three 18 px icon-font
    labels, right-aligned: wifi 172..190, bluetooth
    194..212, battery 216..234 (6 px margin, 4 px gaps).
    Each box must hold icon-gray pixels (a missing font
    or a text label leaves it empty); the wifi box must
    also show gray at rows <= 5 and >= 18, which the
    UI-7 11 px masks (rows 6..16) cannot reach; the
    battery interior must stay empty. Returns (ok, text)."""
    from PIL import Image
    im = Image.open(ppm).convert("RGB")

    def count(x0, x1, y0=0, y1=23):
        return sum(1 for x in range(x0, x1 + 1)
                   for y in range(y0, y1 + 1)
                   if _is_icon_gray(im.getpixel((x, y))))

    def any_gray(x0, x1, y0, y1):
        return any(_is_icon_gray(im.getpixel((x, y)))
                   for x in range(x0, x1 + 1)
                   for y in range(y0, y1 + 1))

    wc = count(172, 190)
    bc = count(194, 212)
    tc = count(216, 234)
    wtop = any_gray(172, 190, 0, 5)
    wbot = any_gray(172, 190, 18, 23)
    iok = not _is_icon_gray(im.getpixel((225, 11)))
    txt = (f"wifi-gray={wc} bt-gray={bc} batt-gray={tc} "
           f"wifi-top={wtop} wifi-bottom={wbot} batt-empty={iok}")
    return wc >= 25 and bc >= 15 and tc >= 15 and wtop and wbot and iok, txt


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

    # ---- Name detail + web note (UI-7) ----
    check("grant starts the shell menu", grant_menu(ser))
    time.sleep(2.0)
    ppm_menu = screendump(mon, "ui5-menu")
    check("menu screendump saved", ppm_menu is not None)
    if ppm_menu is not None:
        ok_icons, icons_txt = icons_ok(ppm_menu)
        print(f"  menu: status-bar dark={bar_dark(ppm_menu)} "
              f"icons: {icons_txt}", flush=True)
        check("menu: status bar renders (clock band)",
              bar_dark(ppm_menu) > 20)
        check("menu: status bar icons render (wifi+bt+battery)",
              ok_icons)
    check("5x down + OK opens Settings",
          goto_entry(ser, 5, "UILV3: settings list", 32))
    time.sleep(1.0)
    ppm_list = screendump(mon, "ui5-list")
    check("settings list screendump saved", ppm_list is not None)
    if ppm_list is not None:
        ok_icons, icons_txt = icons_ok(ppm_list)
        print(f"  list: status-bar dark={bar_dark(ppm_list)} "
              f"icons: {icons_txt}", flush=True)
        check("settings list: status bar renders (clock band)",
              bar_dark(ppm_list) > 20)
        check("settings list: status bar icons render",
              ok_icons)
    pos = len(serial)
    check("OK opens the Device name detail",
          key(ser, "ok", "UILV3: detail name", 32))
    check("name detail focuses BACK first (no editor)",
          wait_from(pos, "UILV3: focus nameback", 32))
    time.sleep(1.5)
    ppm_detail_ab = screendump(mon, "ui5-detail-ab")
    check("name detail screendump saved", ppm_detail_ab is not None)
    if ppm_detail_ab is not None:
        from PIL import Image
        im_detail_ab = Image.open(ppm_detail_ab).convert("RGB")
        _, val_dark, _ = band_stats(ppm_detail_ab, 62, 80)
        _, note_dark, _ = band_stats(ppm_detail_ab, 100, 124)
        _, _, back_blue = band_stats(ppm_detail_ab, 130, 154)
        ok_icons, icons_txt = icons_ok(ppm_detail_ab)
        print(f"  detail AB: value dark={val_dark} note dark={note_dark} "
              f"back blue={back_blue} bar dark={bar_dark(ppm_detail_ab)} "
              f"icons: {icons_txt}", flush=True)
        check("name detail: value rendered (dark)", val_dark > 20)
        check("name detail: web note rendered (dark)", note_dark > 100)
        check("name detail: BACK button rendered (blue)",
              back_blue > 1000)
        check("name detail: status bar renders (clock band)",
              bar_dark(ppm_detail_ab) > 20)
        check("name detail: status bar icons render", ok_icons)
    else:
        im_detail_ab = None
    # Focus is BACK: OK returns to the list, and no
    # editor opens (no UILV5 marker anywhere yet).
    pos = len(serial)
    check("OK on BACK returns to the settings list",
          key(ser, "ok", "UILV3: settings list", 32))
    check("no editor opened from the detail",
          b"UILV5:" not in serial[pos:])

    # ---- Rename round trip via the shell: AB -> ABC ----
    # The device side only displays now; the web
    # config (here: the shell setup command writing
    # the same cfg-name) owns the write.
    release_to_shell(ser)
    check("shell renames AB to ABC",
          shell_cmd(ser, "setup name ABC", "setup: name saved", 10))
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

    # ---- BACK-button parity: detail BACK -> list ----
    check("shell sets name KeepMe",
          shell_cmd(ser, "setup name KeepMe", "setup: name saved", 10))
    check("back: name detail opens", open_name_detail(ser))
    pos = len(serial)
    check("back: OK on BACK returns to the settings list",
          key(ser, "ok", "UILV3: settings list", 32))
    release_to_shell(ser)
    check("back: shell still reads KeepMe",
          shell_cmd(ser, "setup", "  name: KeepMe", 10))

    # ---- 40-char bound (shell write, device display) ----
    bound = "A" * 40
    # Probe first: a long `setup name` burst sent in
    # the racy post-release window has lost chars
    # before (UI-6 flake note); prove the shell is
    # responsive before sending it. The set itself
    # goes chunked: single-send bursts lost chars
    # three times (29 of 40 A's stored); chunking
    # only paces delivery, the stored-value check
    # below still validates the full 40 chars.
    check("bound: shell responsive before long burst",
          shell_probe(ser, "setup", "  name: KeepMe"))
    # Retry-until-readback (UI-8 harness fix, test-only):
    # chunked delivery can still lose a whole 8-char
    # chunk to PL011 RX FIFO overflow under load (the
    # UI-8 debug run stored 32 of 40 A's while the
    # shell still printed "name saved" — it does not
    # validate length), so the set alone cannot prove
    # the value. Retry the set until the readback
    # shows the 39-char prefix (max storable: the
    # 40-byte buffer holds 39 chars + NUL), up to 3
    # attempts. Same verdict checks, same count.
    bound_ok = False
    for _attempt in range(3):
        if not shell_cmd_chunked(ser, "setup name " + bound,
                                 "setup: name saved", 10):
            continue
        release_to_shell(ser)
        if shell_cmd(ser, "setup", "  name: " + "A" * 39, 10):
            bound_ok = True
            break
    check("shell sets a 40-char name", bound_ok)
    check("bound: name detail opens", open_name_detail(ser))
    time.sleep(1.5)
    ppm_bound = screendump(mon, "ui5-detail-bound")
    check("bound: detail screendump saved", ppm_bound is not None)
    if ppm_bound is not None:
        _, bound_dark, _ = band_stats(ppm_bound, 62, 90)
        print(f"  bound detail: value dark={bound_dark}", flush=True)
        check("bound: detail renders the long value (dark)",
              bound_dark > 20)
    release_to_shell(ser)
    check("bound: shell reads back the 40-char prefix",
          bound_ok)
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
        ok_icons, icons_txt = icons_ok(ppm_mon)
        print(f"  monitor: bar dark={bar_dark(ppm_mon)} "
              f"icons: {icons_txt}", flush=True)
        check("polish: monitor status bar renders (clock band)",
              bar_dark(ppm_mon) > 20)
        check("polish: monitor status bar icons render", ok_icons)
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
        ok_icons, icons_txt = icons_ok(ppm_pwr)
        print(f"  power: bar dark={bar_dark(ppm_pwr)} "
              f"icons: {icons_txt}", flush=True)
        check("polish: power status bar renders (clock band)",
              bar_dark(ppm_pwr) > 20)
        check("polish: power status bar icons render", ok_icons)
    check("polish: Power DOWN to RESTART",
          key(ser, "down", "UILV4: focus restart", 32))
    check("polish: Power DOWN to SLEEP",
          key(ser, "down", "UILV4: focus sleep", 32))
    check("polish: Power DOWN to BACK",
          key(ser, "down", "UILV4: focus back", 32))
    check("polish: Power BACK button returns to menu",
          key(ser, "ok", "UILV4: back menu", 32))

    # ---- Polish: idle exit from the detail ----
    check("idle: Settings opens",
          goto_entry(ser, 5, "UILV3: settings list", 32))
    check("idle: name detail opens",
          key(ser, "ok", "UILV3: detail name", 32))
    # One pos for both waits: idle exit and release
    # can arrive in a single reader chunk, so a fresh
    # pos for the release wait would miss a release
    # that already arrived (run-1 lesson).
    pos = len(serial)
    check("idle: detail idles out (30 s guest)",
          wait_from(pos, "UILV2: idle exit", 70))
    check("idle: token released after idle exit",
          wait_from(pos, "uiapp: token released", 32))

    seg = serial.decode("utf-8", "replace")
    check("no kernel FAULT anywhere", "FAULT" not in seg)
    check("no uiapp FAIL anywhere", "uiapp FAIL" not in seg)
    check("no UILV2 flush failures", "UILV2: flush failures" not in seg)
    check("no UILV5 markers anywhere (editor removed)",
          "UILV5:" not in seg)
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
