#!/usr/bin/env python3
"""UI1-VERIFY: LVGL v9.5.0 vendor + port + measure (UI-1).

The LVGL smoke screen is marker-gated: uiapp runs it
instead of the menu only when /data/ui1-test reads "1"
(see user/uiapp/lv_port.c). This script boots QEMU,
drives the standard flow (setup reset -> gui -> setup
done -> marker -> key menu), and checks:

  * UILV: start appears after the token grant;
  * screendump 1 shows the LVGL screen: AA title text
    (dark glyph pixels), two blue theme buttons, READY
    status text;
  * keypad focus moves: key down -> "UILV: focus back",
    key up -> "UILV: focus press" (DOWN maps to
    LV_KEY_NEXT, UP to LV_KEY_PREV: LVGL keypad groups
    move focus ONLY on NEXT/PREV, arrows are object
    data -- see DEVLOG-UI1);
  * key ok on PRESS ME -> "UILV: activated", and
    screendump 2 differs from dump 1 ONLY in the status
    band (READY -> ACTIVATED), i.e. the redraw is
    invalidation-scoped, not a full repaint;
  * key back -> "UILV: exit", marker cleared, disk
    left unprovisioned (setup reset), no QEMU left;
  * no FAULT, no uiapp FAIL, no UILV flush FAIL lines
    (the C6L-5 face-side SYS_DISPLAY_FLUSH failure is
    tracked here: it must not reproduce on the uiapp
    LVGL path);
  * size evidence: uiapp.bin size and objcopy-binary
    headroom are printed (method: llvm-objcopy-18 -O
    binary on the linked mach3.elf vs the 4,194,304 B
    boot partition, same as C6L-3/4/5).

Pre-change shape: on the C6L-5 kernel there is no
UILV marker, no LVGL screen, and no /data/ui1-test
reader -- the script FAILs on every UILV check. Set
KERNEL to that ELF to reproduce.

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first;
scratch lives under ~/workspace/logs-ui1 (NOT /tmp).
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
LOGDIR = os.path.expanduser("~/workspace/logs-ui1")
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


SER = os.path.join(LOGDIR, "ui1-ser.sock")
MON = os.path.join(LOGDIR, "ui1-mon.sock")
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


def set_marker(ser, val, tries=3):
    # put + read-back must agree (C6L-5 garbled-put
    # lesson: a bare "put: ok" is not proof).
    want = ("\n" + val + "\n").encode()
    for _ in range(tries):
        if not shell_cmd(ser, "put /data/ui1-test " + val,
                         "put: ok", 20):
            continue
        if shell_cmd(ser, "cat /data/ui1-test", want.decode(), 20):
            return True
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
    """Full-frame near-white count. The LVGL default
    theme paints a white screen; the hand-rolled menu
    paints a near-black one (0 white pixels on the
    base kernel) -- so this is the check that tells
    the LVGL screen apart from the menu, where the
    title/button/status band checks alone do not."""
    from PIL import Image
    im = Image.open(ppm).convert("RGB")
    n = 0
    for yy in range(240):
        for xx in range(240):
            r, g, b = im.getpixel((xx, yy))
            if r > 240 and g > 240 and b > 240:
                n += 1
    return n


def band_diff(a, b, y0, y1):
    n = 0
    for yy in range(y0, y1):
        for xx in range(240):
            if a.getpixel((xx, yy)) != b.getpixel((xx, yy)):
                n += 1
    return n


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
    check("marker set and reads back 1", set_marker(ser, "1"))

    pos = len(serial)
    ser.sendall(b"key menu\n")
    check("UILV: start after token grant",
          wait_from(pos, "UILV: start", 60))
    time.sleep(2.0)
    ppm1 = screendump(mon, "ui1-dump1")
    check("screendump 1 saved", ppm1 is not None)
    if ppm1 is not None:
        im1, title_dark, _ = band_stats(ppm1, 16, 34)
        _, status_dark1, _ = band_stats(ppm1, 188, 208)
        _, _, btn1_blue = band_stats(ppm1, 64, 110)
        _, _, btn2_blue = band_stats(ppm1, 124, 170)
        w1 = white_pixels(ppm1)
        print(f"  dump1: title dark={title_dark} status dark={status_dark1} "
              f"btn1 blue={btn1_blue} btn2 blue={btn2_blue} white={w1}",
              flush=True)
        check("dump1: light LVGL-theme background (not the dark menu)",
              w1 > 20000)
        check("dump1: AA title text rendered", title_dark > 50)
        check("dump1: PRESS ME button rendered (blue)", btn1_blue > 1000)
        check("dump1: BACK button rendered (blue)", btn2_blue > 1000)
        check("dump1: READY status text rendered", status_dark1 > 50)
    else:
        im1 = None

    pos = len(serial)
    ser.sendall(b"key down\n")
    check("key down moves focus to BACK",
          wait_from(pos, "UILV: focus back", 20))
    pos = len(serial)
    ser.sendall(b"key up\n")
    check("key up moves focus back to PRESS ME",
          wait_from(pos, "UILV: focus press", 20))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("key ok activates PRESS ME",
          wait_from(pos, "UILV: activated", 20))
    time.sleep(2.0)
    ppm2 = screendump(mon, "ui1-dump2")
    check("screendump 2 saved", ppm2 is not None)
    if ppm1 is not None and ppm2 is not None:
        im2, status_dark2, _ = band_stats(ppm2, 188, 208)
        sd = band_diff(im1, im2, 188, 208)
        fd = band_diff(im1, im2, 0, 240)
        print(f"  dump2: status dark={status_dark2} status-band diff={sd} "
              f"full-frame diff={fd}", flush=True)
        check("dump2: ACTIVATED status text rendered", status_dark2 > 50)
        check("dump2: status band changed (READY->ACTIVATED)", sd > 200)
        check("dump2: change is scoped to the status band",
              fd == sd)
    pos = len(serial)
    ser.sendall(b"key back\n")
    check("key back exits LVGL smoke run",
          wait_from(pos, "UILV: exit", 20))

    seg = serial.decode("utf-8", "replace")
    check("no kernel FAULT anywhere", "FAULT" not in seg)
    check("no uiapp FAIL anywhere", "uiapp FAIL" not in seg)
    check("no UILV flush FAIL lines", "UILV: flush FAIL" not in seg)

    # Teardown: marker cleared, disk unprovisioned.
    check("marker cleared and reads back 0", set_marker(ser, "0"))
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(1.0)

    # Size evidence (objcopy-binary method, C6L-3/4/5).
    ubin = os.path.join(ROOT, "mach3-port/build/uiapp.bin")
    kbin = os.path.join(LOGDIR, "ui1-kernel.bin")
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
        with open(os.path.join(LOGDIR, "ui1-serial.log"), "wb") as f:
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
print(f"UI1-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
