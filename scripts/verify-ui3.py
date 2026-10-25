#!/usr/bin/env python3
"""UI3-VERIFY: LVGL Settings screen (UI-3).

The shell's Settings entry is a real LVGL submenu (no
longer the UI-2 placeholder): Device name, Timezone,
PIN. This script boots QEMU, drives the standard flow
(setup reset -> gui -> setup done), sets known state
via the shell (setup name/tz/pin), then checks:

  * the shell menu still renders (UILV2: start, light
    dump) and 5x down reaches Settings;
  * OK on Settings opens the settings list
    ("UILV3: settings list", blue buttons, differs
    from the menu) and down/up walks every entry;
  * OK on Device name shows the current value
    ("UILV3: detail name", value set via the shell)
    with an honest "edit in UI-5" note, BACK
    returns to the list;
  * OK on Timezone shows the option list
    ("UILV3: detail tz", focus starts on the current
    value), DOWN + OK saves +0800 ("UILV3: tz saved
    +0800"), and the shell `setup` status reads back
    +0800 (persistence through an independent
    channel) — a re-grant then focuses +0800;
  * OK on PIN shows the status ("UILV3: detail pin",
    Set) and the Change PIN digit-picker flow:
    old 1234 verifies ("UILV3: pin verified"), new
    5678 set + confirmed ("UILV3: pin changed");
    a wrong old PIN reports "UILV3: pin wrong"
    (exactly one fail, no lockout); changing back
    with old 5678 proves the stored value really is
    5678 (digit assembly correct), restoring 1234;
  * BACK routing at every level (detail -> list ->
    menu -> release) and the 30 s idle contract is
    untouched (covered by UI2-VERIFY);
  * no FAULT, no uiapp FAIL, no UILV2/UILV3 flush
    failures; teardown leaves the disk unprovisioned
    and no QEMU running;
  * size evidence: uiapp.bin size and objcopy-binary
    headroom are printed (method: llvm-objcopy-18 -O
    binary on the linked mach3.elf vs the 4,194,304 B
    boot partition, same as C6L-3/4/5/UI-1/UI-2).

Pre-change shape: on the UI-2 kernel Settings is
still the honest placeholder ("UILV2: placeholder
settings", no UILV3 marker) -- the script FAILs on
every UILV3 check. Set KERNEL to that ELF to
reproduce. Set UI3_TSCALE < 1 to shorten waits.

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first;
scratch lives under ~/workspace/logs-ui3 (NOT /tmp).
"""
import os
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get("KERNEL", ROOT + "/mach3-port/build/mach3.elf")
TSCALE = float(os.environ.get("UI3_TSCALE", "1.0"))
LOGDIR = os.path.expanduser("~/workspace/logs-ui3")
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


SER = os.path.join(LOGDIR, "ui3-ser.sock")
MON = os.path.join(LOGDIR, "ui3-mon.sock")
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
    pos = len(serial)
    ser.sendall(f"key {k}\n".encode())
    return wait_from(pos, needle, timeout)


def send_keys(ser, seq, delay=0.35):
    """Raw key sequence with pacing; no per-key marker."""
    for k in seq:
        ser.sendall(f"key {k}\n".encode())
        time.sleep(delay * TSCALE)


def pin_entry(ser, digits):
    """Digit-picker entry: digits are 0-9 ints. The picker
    starts each position at 0; UP cycles, OK advances
    (OK on the last position confirms)."""
    seq = []
    for d in digits:
        seq += ["up"] * d
        seq += ["ok"]
    send_keys(ser, seq)


def grant_menu(ser):
    """Fresh token grant -> shell menu. Returns False
    unless UILV2: start appears."""
    pos = len(serial)
    ser.sendall(b"key menu\n")
    return wait_from(pos, "UILV2: start", 60)


def goto_settings(ser):
    """From the shell menu (focus Qabot): 5x down to
    Settings, OK -> settings list. Returns False
    unless the list marker appears."""
    # Walk down one entry at a time; the last down must
    # land on settings (focus markers prove the path).
    for want in ("wifi", "ble", "llm", "passkey", "settings"):
        if not key(ser, "down", "UILV2: focus " + want, 20):
            return False
    pos = len(serial)
    ser.sendall(b"key ok\n")
    return wait_from(pos, "UILV3: settings list", 20)


def release_menu(ser):
    """From the settings list: BACK -> menu, BACK at
    root -> release. Returns False unless released."""
    pos = len(serial)
    ser.sendall(b"key back\n")
    if not wait_from(pos, "UILV3: back menu", 20):
        return False
    pos = len(serial)
    ser.sendall(b"key back\n")
    if not wait_from(pos, "UILV2: release", 20):
        return False
    return wait_from(pos, "uiapp: token released", 20)


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

    # Known state via the shell (independent channel).
    check("shell sets device name",
          shell_cmd(ser, "setup name TestDev", "setup: name saved", 10))
    check("shell sets timezone",
          shell_cmd(ser, "setup tz +0700", "setup: tz saved", 10))
    check("shell sets PIN",
          shell_cmd(ser, "setup pin 1234", "setup: PIN saved", 10))

    check("grant starts the shell menu", grant_menu(ser))
    time.sleep(2.0)
    ppm_menu = screendump(mon, "ui3-menu")
    check("menu screendump saved", ppm_menu is not None)
    if ppm_menu is not None:
        w = white_pixels(ppm_menu)
        print(f"  menu: white={w}", flush=True)
        check("menu renders light (LVGL shell)", w > 5000)

    check("5x down + OK opens the settings list",
          goto_settings(ser))
    time.sleep(2.0)
    ppm_list = screendump(mon, "ui3-list")
    check("settings list screendump saved", ppm_list is not None)
    if ppm_menu is not None and ppm_list is not None:
        from PIL import Image
        im_list, _, btn1_blue = band_stats(ppm_list, 24, 48)
        _, _, btn4_blue = band_stats(ppm_list, 102, 126)
        fd = frame_diff(Image.open(ppm_menu).convert("RGB"), im_list)
        print(f"  list: btn1 blue={btn1_blue} btn4 blue={btn4_blue} "
              f"frame diff={fd}", flush=True)
        check("list: first entry button rendered (blue)",
              btn1_blue > 1000)
        check("list: BACK button rendered (blue)", btn4_blue > 1000)
        check("list: differs from the menu", fd > 1000)
    else:
        im_list = None

    # Walk every list entry and back.
    check("list: down moves focus to Timezone",
          key(ser, "down", "UILV3: focus tz", 20))
    check("list: down moves focus to PIN",
          key(ser, "down", "UILV3: focus pin", 20))
    check("list: down moves focus to BACK",
          key(ser, "down", "UILV3: focus setback", 20))
    check("list: up moves focus back to PIN",
          key(ser, "up", "UILV3: focus pin", 20))
    check("list: up moves focus back to Timezone",
          key(ser, "up", "UILV3: focus tz", 20))
    check("list: up moves focus back to Device name",
          key(ser, "up", "UILV3: focus name", 20))

    # Device name detail: current value + UI-5 note.
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK opens the Device name detail",
          wait_from(pos, "UILV3: detail name", 20))
    time.sleep(2.0)
    ppm_name = screendump(mon, "ui3-name")
    check("name detail screendump saved", ppm_name is not None)
    if ppm_name is not None and im_list is not None:
        from PIL import Image
        _, _, back_blue = band_stats(ppm_name, 130, 174)
        fd = frame_diff(im_list, Image.open(ppm_name).convert("RGB"))
        print(f"  name: BACK blue={back_blue} frame diff={fd}", flush=True)
        check("name: BACK button rendered (blue)", back_blue > 1000)
        check("name: differs from the list", fd > 1000)
    pos = len(serial)
    ser.sendall(b"key back\n")
    check("BACK returns to the settings list",
          wait_from(pos, "UILV3: settings list", 20))

    # Timezone detail: focus starts on the current value.
    check("down moves focus to Timezone",
          key(ser, "down", "UILV3: focus tz", 20))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK opens the Timezone detail",
          wait_from(pos, "UILV3: detail tz", 20))
    check("tz detail focuses the current value (+0700)",
          wait_from(pos, "UILV3: focus tz+0700", 20))
    time.sleep(2.0)
    ppm_tz = screendump(mon, "ui3-tz")
    check("tz detail screendump saved", ppm_tz is not None)
    if ppm_tz is not None:
        _, _, opt1_blue = band_stats(ppm_tz, 48, 68)
        _, _, opt6_blue = band_stats(ppm_tz, 158, 178)
        print(f"  tz: opt1 blue={opt1_blue} opt6 blue={opt6_blue}",
              flush=True)
        check("tz: first option rendered (blue)", opt1_blue > 1000)
        check("tz: last option rendered (blue)", opt6_blue > 1000)
    check("down moves focus to +0800",
          key(ser, "down", "UILV3: focus tz+0800", 20))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK saves +0800",
          wait_from(pos, "UILV3: tz saved +0800", 20))
    time.sleep(1.0)
    ppm_tz2 = screendump(mon, "ui3-tz2")
    check("tz saved screendump saved", ppm_tz2 is not None)
    if ppm_tz2 is not None:
        _, title_dark, _ = band_stats(ppm_tz2, 24, 40)
        print(f"  tz2: title dark={title_dark}", flush=True)
        check("tz: title still rendered after save", title_dark > 100)
    pos = len(serial)
    ser.sendall(b"key back\n")
    check("BACK returns to the settings list",
          wait_from(pos, "UILV3: settings list", 20))

    # Timezone persistence via the shell (independent
    # channel) after a full release + re-grant.
    check("release from the settings list", release_menu(ser))
    check("shell status reads back +0800",
          shell_cmd(ser, "setup", "tz: +0800", 10))
    check("re-grant starts the shell menu", grant_menu(ser))
    check("re-grant reaches the settings list", goto_settings(ser))
    check("down moves focus to Timezone",
          key(ser, "down", "UILV3: focus tz", 20))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK reopens the Timezone detail",
          wait_from(pos, "UILV3: detail tz", 20))
    check("tz detail focuses +0800 after re-grant",
          wait_from(pos, "UILV3: focus tz+0800", 20))
    pos = len(serial)
    ser.sendall(b"key back\n")
    check("BACK returns to the settings list",
          wait_from(pos, "UILV3: settings list", 20))
    check("release from the settings list", release_menu(ser))

    # PIN detail + full change round trip.
    check("re-grant starts the shell menu", grant_menu(ser))
    check("re-grant reaches the settings list", goto_settings(ser))
    check("down moves focus to Timezone",
          key(ser, "down", "UILV3: focus tz", 20))
    check("down moves focus to PIN",
          key(ser, "down", "UILV3: focus pin", 20))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK opens the PIN detail",
          wait_from(pos, "UILV3: detail pin", 20))
    time.sleep(2.0)
    ppm_pin = screendump(mon, "ui3-pin")
    check("pin detail screendump saved", ppm_pin is not None)
    if ppm_pin is not None:
        _, _, back_blue = band_stats(ppm_pin, 140, 164)
        print(f"  pin: BACK blue={back_blue}", flush=True)
        check("pin: BACK button rendered (blue)", back_blue > 1000)
    # Change PIN: old 1234 -> new 5678 (confirm 5678).
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK opens the PIN length select",
          wait_from(pos, "UILV3: pin len old", 20))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK confirms length 4, old entry opens",
          wait_from(pos, "UILV3: pin entry old", 20))
    pin_entry(ser, [1, 2, 3, 4])
    check("old PIN 1234 verifies",
          wait_from(pos, "UILV3: pin verified", 30))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK confirms length 4, new entry opens",
          wait_from(pos, "UILV3: pin entry new", 20))
    pin_entry(ser, [5, 6, 7, 8])
    check("new PIN entry opens confirmation",
          wait_from(pos, "UILV3: pin entry confirm", 30))
    pin_entry(ser, [5, 6, 7, 8])
    check("new PIN 5678 set and confirmed",
          wait_from(pos, "UILV3: pin changed", 30))
    time.sleep(1.0)
    # Wrong old PIN: exactly one fail, no lockout.
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK reopens the PIN length select",
          wait_from(pos, "UILV3: pin len old", 20))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK confirms length 4, old entry opens",
          wait_from(pos, "UILV3: pin entry old", 20))
    pin_entry(ser, [1, 2, 3, 4])
    check("old PIN 1234 now reports wrong",
          wait_from(pos, "UILV3: pin wrong", 30))
    # Change back with old 5678: proves the stored
    # value really is 5678 (digit assembly correct).
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK reopens the PIN length select",
          wait_from(pos, "UILV3: pin len old", 20))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK confirms length 4, old entry opens",
          wait_from(pos, "UILV3: pin entry old", 20))
    pin_entry(ser, [5, 6, 7, 8])
    check("old PIN 5678 verifies",
          wait_from(pos, "UILV3: pin verified", 30))
    pos = len(serial)
    ser.sendall(b"key ok\n")
    check("OK confirms length 4, new entry opens",
          wait_from(pos, "UILV3: pin entry new", 20))
    pin_entry(ser, [1, 2, 3, 4])
    check("new PIN entry opens confirmation",
          wait_from(pos, "UILV3: pin entry confirm", 30))
    pin_entry(ser, [1, 2, 3, 4])
    check("PIN restored to 1234",
          wait_from(pos, "UILV3: pin changed", 30))
    pos = len(serial)
    ser.sendall(b"key back\n")
    check("BACK returns to the settings list",
          wait_from(pos, "UILV3: settings list", 20))
    check("release from the settings list", release_menu(ser))
    check("shell status still shows PIN set",
          shell_cmd(ser, "setup", "pin: set", 10))

    seg = serial.decode("utf-8", "replace")
    check("no kernel FAULT anywhere", "FAULT" not in seg)
    check("no uiapp FAIL anywhere", "uiapp FAIL" not in seg)
    check("no UILV2 flush failures", "UILV2: flush failures" not in seg)
    check("no UILV3 flush failures", "UILV3: flush failures" not in seg)

    # Teardown: disk unprovisioned.
    shell_cmd(ser, "setup reset", "", 10)
    time.sleep(1.0)

    # Size evidence (objcopy-binary method, C6L-3/4/5/UI-1/UI-2).
    ubin = os.path.join(ROOT, "mach3-port/build/uiapp.bin")
    kbin = os.path.join(LOGDIR, "ui3-kernel.bin")
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
        with open(os.path.join(LOGDIR, "ui3-serial.log"), "wb") as f:
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
print(f"UI3-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
