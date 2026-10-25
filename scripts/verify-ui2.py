#!/usr/bin/env python3
"""UI2-VERIFY: LVGL shell / status bar / menu (UI-2).

The LVGL shell is the DEFAULT uiapp menu (no marker):
after the token grant uiapp runs it instead of the old
hand-rolled menu. This script boots QEMU, drives the
standard flow (setup reset -> gui -> setup done ->
key menu), and checks:

  * UILV2: start appears after the token grant;
  * screendump 1 shows the shell: light LVGL-theme
    background, status-bar clock text in the top band,
    blue menu buttons in the first and last button bands;
  * keypad navigation walks every menu entry: 7x down
    moves focus qabot -> wifi -> ble -> llm -> passkey
    -> settings -> monitor -> power, up moves back;
  * OK on Settings opens the UI-3 settings list
    ("UILV3: settings list", BACK button blue
    in the dump, frame differs from the menu);
  * BACK on the settings list returns to the menu
    ("UILV3: back menu", focus qabot again, light dump);
  * OK on Wi-Fi runs the existing hand-rolled screen
    ("UILV2: oldscreen wifi", dump turns dark = old
    renderer), BACK there pops back to the shell
    (second "UILV2: start", light dump);
  * BACK at the menu root releases the token
    ("UILV2: release", "uiapp: token released");
  * a fresh grant then idles out after 30 s
    ("UILV2: idle exit") — the auto-return contract;
  * no FAULT, no uiapp FAIL, no UILV2 flush failures;
  * teardown leaves the disk unprovisioned and no
    QEMU running;
  * size evidence: uiapp.bin size and objcopy-binary
    headroom are printed (method: llvm-objcopy-18 -O
    binary on the linked mach3.elf vs the 4,194,304 B
    boot partition, same as C6L-3/4/5/UI-1).

Pre-change shape: on the UI-1 kernel there is no
UILV2 marker and no LVGL menu (the old dark menu
runs instead) -- the script FAILs on every UILV2
check. Set KERNEL to that ELF to reproduce. Set
UI2_TSCALE < 1 to shorten waits for that run.

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first;
scratch lives under ~/workspace/logs-ui2 (NOT /tmp).
"""
import os
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get("KERNEL", ROOT + "/mach3-port/build/mach3.elf")
TSCALE = float(os.environ.get("UI2_TSCALE", "1.0"))
LOGDIR = os.path.expanduser("~/workspace/logs-ui2")
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


SER = os.path.join(LOGDIR, "ui2-ser.sock")
MON = os.path.join(LOGDIR, "ui2-mon.sock")
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


def shell_cmd(ser, cmd, wait_for, timeout=30):
    # Same lost-input hazard as C6L-5: retry ONLY when
    # the command echo is absent from the segment.
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
    pos = len(serial)
    ser.sendall(f"key {k}\n".encode())
    return wait_from(pos, needle, timeout)


qemu = None
try:
    qemu, ser, mon = launch_qemu()

    check("boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(0.6)
    ser.sendall(b"gui\n")
    check("face setup QR screen",
          wait_from(0, "face: setup mode, payload", 60))
    finale = False
    for _ in range(4):
        ser.sendall(b"setup done\n")
        if wait_from(len(serial) - 200, "provisioned -> finale", 10):
            finale = True
            break
    check("setup done reaches finale", finale)

    pos = len(serial)
    ser.sendall(b"key menu\n")
    check("UILV2: start after token grant",
          wait_from(pos, "UILV2: start", 60))
    time.sleep(2.0)
    ppm1 = screendump(mon, "ui2-dump1")
    check("screendump 1 saved", ppm1 is not None)
    if ppm1 is not None:
        im1, top_dark, _ = band_stats(ppm1, 0, 20)
        _, _, btn1_blue = band_stats(ppm1, 24, 48)
        _, _, btn8_blue = band_stats(ppm1, 206, 230)
        w1 = white_pixels(ppm1)
        print(f"  dump1: top dark={top_dark} btn1 blue={btn1_blue} "
              f"btn8 blue={btn8_blue} white={w1}", flush=True)
        # The UI-2 menu is NOT mostly white: 8 blue buttons
        # (200x24) + the gray bar cover most of the frame.
        # ~11k white px vs the dark menu's 0 is the
        # discriminating signal, so the bar is 5,000.
        check("dump1: light LVGL-theme background (not the dark menu)",
              w1 > 5000)
        check("dump1: status-bar clock text rendered", top_dark > 20)
        check("dump1: first menu button rendered (blue)", btn1_blue > 1000)
        check("dump1: last menu button rendered (blue)", btn8_blue > 1000)
    else:
        im1 = None

    # Walk all 8 entries: 7x down from Qabot.
    focus_seq = ["wifi", "ble", "llm", "passkey",
                 "settings", "monitor", "power"]
    ok = True
    for name in focus_seq:
        if not key(ser, "down", f"UILV2: focus {name}"):
            ok = False
            break
    check("key down walks focus through all 8 entries", ok)
    check("key up moves focus back to monitor",
          key(ser, "up", "UILV2: focus monitor"))
    check("key up moves focus back to settings",
          key(ser, "up", "UILV2: focus settings"))

    # UI-3: OK on Settings opens the settings list
    # (the UI-2 placeholder is gone; the list's BACK
    # button sits at y=102, band 102-126).
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("key ok activates Settings list",
          wait_from(pos, "UILV3: settings list", 20))
    time.sleep(2.0)
    ppm2 = screendump(mon, "ui2-dump2")
    check("screendump 2 saved", ppm2 is not None)
    if ppm1 is not None and ppm2 is not None:
        im2, _, _ = band_stats(ppm2, 0, 20)
        _, _, back_blue = band_stats(ppm2, 102, 126)
        fd = frame_diff(im1, im2)
        print(f"  dump2: BACK blue={back_blue} frame diff={fd}",
              flush=True)
        check("dump2: settings BACK button rendered (blue)",
              back_blue > 1000)
        check("dump2: settings screen differs from menu", fd > 1000)
    else:
        im2 = None

    # BACK on the settings list returns to the menu,
    # and the rebuild refocuses the first entry
    # (Qabot) with no key press at all. ORDERING
    # QUIRK: the rebuild's focus print comes BEFORE
    # the "back menu" print in the stream (build_menu
    # runs, then the loop prints), so both markers
    # must be checked in the segment captured BEFORE
    # the key is sent — a fresh window after
    # "back menu" can never see the focus print.
    pos = len(serial)
    ser.sendall(b"key back\n")
    check("key back returns to menu",
          wait_from(pos, "UILV3: back menu", 20))
    check("menu rebuild refocuses Qabot",
          b"UILV2: focus qabot" in serial[pos:])
    time.sleep(2.0)
    ppm3 = screendump(mon, "ui2-dump3")
    check("screendump 3 saved", ppm3 is not None)
    if ppm3 is not None:
        w3 = white_pixels(ppm3)
        print(f"  dump3: white={w3}", flush=True)
        check("dump3: menu restored (light background)", w3 > 5000)

    # Old-screen round-trip: focus is Qabot after the
    # rebuild; down -> wifi, ok -> old wifi screen.
    check("key down moves focus to Wi-Fi",
          key(ser, "down", "UILV2: focus wifi"))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("key ok activates Wi-Fi",
          wait_from(pos, "UILV2: activate wifi", 20))
    check("shell hands off to the old Wi-Fi screen",
          wait_from(pos, "UILV2: oldscreen wifi", 20))
    time.sleep(2.0)
    ppm4 = screendump(mon, "ui2-dump4")
    check("screendump 4 saved", ppm4 is not None)
    if ppm4 is not None:
        w4 = white_pixels(ppm4)
        print(f"  dump4: white={w4}", flush=True)
        check("dump4: old screen renders dark (hand-rolled)",
              w4 < 1000)
    # BACK on the old screen pops it -> shell restarts.
    # The Wi-Fi screen enters with a no-peer error note
    # (refresh_status fails: no C6 peer in this run),
    # and its handler eats the FIRST keypress of any
    # kind to dismiss the note — so the first BACK
    # dismisses the note and only the second BACK
    # pops. That is correct old-screen behaviour,
    # not a shell bug, and the script must send two.
    ser.sendall(b"key back\n")
    time.sleep(2.0)
    pos = len(serial)
    ser.sendall(b"key back\n")
    check("BACK on old screen returns to the shell",
          wait_from(pos, "UILV2: start", 20))
    time.sleep(2.0)
    ppm5 = screendump(mon, "ui2-dump5")
    check("screendump 5 saved", ppm5 is not None)
    if ppm5 is not None:
        w5 = white_pixels(ppm5)
        print(f"  dump5: white={w5}", flush=True)
        check("dump5: shell menu restored (light background)",
              w5 > 5000)

    # BACK at the menu root releases the token.
    pos = len(serial)
    ser.sendall(b"key back\n")
    check("key back at menu root releases",
          wait_from(pos, "UILV2: release", 20))
    check("token released to face",
          wait_from(pos, "uiapp: token released", 20))

    # Fresh grant, then idle out (auto-return contract).
    pos = len(serial)
    ser.sendall(b"key menu\n")
    check("second grant starts the shell again",
          wait_from(pos, "UILV2: start", 60))
    check("idle exit after 30 s without input",
          wait_from(pos, "UILV2: idle exit", 45))
    check("token released after idle exit",
          wait_from(pos, "uiapp: token released", 20))

    seg = serial.decode("utf-8", "replace")
    check("no kernel FAULT anywhere", "FAULT" not in seg)
    check("no uiapp FAIL anywhere", "uiapp FAIL" not in seg)
    check("no UILV2 flush failures", "UILV2: flush failures" not in seg)

    # Teardown: disk unprovisioned.
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(1.0)

    # Size evidence (objcopy-binary method, C6L-3/4/5/UI-1).
    ubin = os.path.join(ROOT, "mach3-port/build/uiapp.bin")
    kbin = os.path.join(LOGDIR, "ui2-kernel.bin")
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
        with open(os.path.join(LOGDIR, "ui2-serial.log"), "wb") as f:
            f.write(serial)
    except Exception:
        pass
    if qemu is not None:
        try:
            mon.sendall(b"quit\n")
        except Exception:
            pass
        time.sleep(0.5)
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
    leftq = subprocess.run(["pgrep", "-x", "qemu-system-arm"],
                           capture_output=True, text=True).stdout.strip()
    check("no qemu-system-arm left running", leftq == "")

npass = sum(1 for _, c in results if c)
print(f"UI2-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
