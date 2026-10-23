#!/usr/bin/env python3
"""FND1-VERIFY: SYS_RESET / SYS_POWEROFF (FND-1).

Checks:
  (a) boot to the qaon> prompt;
  (b) shell `reboot` -> a second boot on the same disk with
      boot_count incremented by 1, kernel boot line reason=4
      (software request), `bootctl` last_reason=4, and a
      `bootlog` record "ev=12 ... software reset" stamped
      with the requesting boot's number;
  (c) GUI: Power screen renders with the Restart item
      (screendump saved to logs/fnd1-power.png; per-row
      glyph-band analysis printed);
  (d) LAST, on a separate boot: shell `poweroff` -> the
      QEMU process exits on its own with rc 0.

Harness rules (learned the hard way, see verify-onb1.py):
the guest console printf is polling-mode, so the serial
socket is drained continuously by a reader thread, and any
leftover qemu-system-arm is killed by exact name first
(a stale QEMU holds the disk write lock and causes
phantom failures).
"""
import os, re, socket, subprocess, sys, threading, time

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

SER = "/tmp/fnd1-ser.sock"
MON = "/tmp/fnd1-mon.sock"
for s in (SER, MON):
    try:
        os.unlink(s)
    except FileNotFoundError:
        pass

results = []


def check(name, cond):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name, flush=True)


def boot_qemu():
    return subprocess.Popen([
        "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7",
        "-m", "64",
        "-L", tc + "/usr/share/qemu",
        "-kernel", ROOT + "/kernel/build/mach3.elf",
        "-serial", f"unix:{SER},server,nowait",
        "-monitor", f"unix:{MON},server,nowait",
        "-device", "virtio-gpu-device", "-display", "none",
        "-drive", f"file={ROOT}/disk-images/mach3-blk.img,if=none,id=hd0,format=raw",
        "-device", "virtio-blk-device,drive=hd0",
        "-drive", f"file={ROOT}/disk-images/sd128.img,if=none,id=hd1,format=raw",
        "-device", "virtio-blk-device,drive=hd1",
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)


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
    """All (boot_count, reason) pairs from kernel boot lines."""
    return [(int(n), int(r)) for n, r in re.findall(
        rb"bootstate: boot #(\d+) fail=\d+ reason=(\d+)", serial)]


# ----------------------------------------------------------------
# Phase A+B: boot, reboot, reason checks.
p = boot_qemu()
ser = conn(SER)
ser.settimeout(0.3)
start_reader(ser)
mon = conn(MON)
mon.settimeout(5)
try:
    mon.recv(4096)
except Exception:
    pass

ok_prompt = wait_from(0, "qaon>", 150)
check("boot reaches qaon> prompt", ok_prompt)
boots = boot_lines()
n1 = boots[-1][0] if boots else -1
print(f"  first boot_count={n1}", flush=True)

pos = len(serial)
ser.sendall(b"reboot\n")
t0 = time.time()
second = False
while time.time() - t0 < 90:
    bl = boot_lines()
    if len(bl) > len(boots):
        second = True
        break
    time.sleep(0.3)
bl = boot_lines()
n2, r2 = bl[-1] if bl else (-1, -1)
print(f"  second boot_count={n2} reason={r2}", flush=True)
check("reboot: second boot on same disk", second)
check("reboot: boot_count incremented by 1", second and n2 == n1 + 1)
check("reboot: kernel boot line reason=4 (software)", second and r2 == 4)
check("reboot: kernel logged SYS_RESET",
      "sys: SYS_RESET (software request)" in txt()[pos:])

ok_prompt2 = wait_from(len(serial), "qaon>", 90) if second else False
if second:
    # The second boot prints its own prompt; wait for any
    # prompt after the second boot line.
    t0 = time.time()
    while time.time() - t0 < 90:
        seg = txt()
        idx = seg.rfind("bootstate: boot #")
        if idx >= 0 and "qaon>" in seg[idx:]:
            ok_prompt2 = True
            break
        time.sleep(0.3)
check("second boot reaches qaon> prompt", ok_prompt2)

pos = len(serial)
ser.sendall(b"bootctl\n")
wait_from(pos, "last_reason=", 20)
seg = txt()[pos:]
m = re.search(r"last_reason=(\d+)", seg)
check("bootctl shows last_reason=4",
      bool(m) and m.group(1) == "4")

pos = len(serial)
ser.sendall(b"bootlog\n")
wait_from(pos, "records", 20)
seg = txt()[pos:]
want = f"boot #{n1} ev=12"
check("bootlog has ev=12 'software reset' for the requesting boot",
      want in seg and "software reset" in seg)
for line in seg.splitlines():
    if "ev=12" in line:
        print("  bootlog record:", line.strip(), flush=True)

# ----------------------------------------------------------------
# Phase C: GUI Power screen (same boot). Drive the standard
# flow: unprovision -> gui (QR) -> setup done (finale) ->
# menu -> POWER tile -> Power screen.
send_pos = len(serial)
ser.sendall(b"setup reset\n")
time.sleep(0.6)
ser.sendall(b"gui\n")
qr_ok = wait_from(send_pos, "face: setup mode, payload", 60)
print("  QR setup screen:", qr_ok, flush=True)

finale_ok = False
for _attempt in range(4):
    ser.sendall(b"setup done\n")
    if wait_from(len(serial) - 200, "provisioned -> finale", 10):
        finale_ok = True
        break
print("  finale reached:", finale_ok, flush=True)

ser.sendall(b"key menu\n")
time.sleep(1.5)
ser.sendall(b"key menu\n")
time.sleep(2.5)
ser.sendall(b"key down\n")     # menu sel 0 -> 2 (SETTINGS row)
time.sleep(0.8)
ser.sendall(b"key right\n")    # 2 -> 3 (POWER)
time.sleep(0.8)
ser.sendall(b"key ok\n")       # open Power screen
time.sleep(2.5)

ppm = os.path.join(ROOT, "logs", "fnd1-power.ppm")
mon.sendall(f"screendump {ppm}\n".encode())
time.sleep(1.0)
try:
    mon.recv(4096)
except Exception:
    pass
png = ppm.replace(".ppm", ".png")
dump_ok = os.path.exists(ppm)
bands = []
if dump_ok:
    from PIL import Image
    im = Image.open(ppm).convert("RGB")
    im.save(png)
    # Item rows in scr_power.c: Y0=58, LH=30 -> labels at
    # y = 58, 88, 118, 148 (SHUTDOWN/RESTART/SLEEP NOW/BACK).
    # Count bright (glyph) pixels in each label band; the
    # face/menu use a near-black background (8,12,24).
    for i, y in enumerate((58, 88, 118, 148)):
        n = 0
        for yy in range(y, y + 9):
            for xx in range(60, 181):
                r, g, b = im.getpixel((xx, yy))
                if r + g + b > 200:
                    n += 1
        bands.append(n)
    print("  label-band glyph pixels (SHUTDOWN/RESTART/SLEEP/BACK):",
          bands, flush=True)
check("power screen screendump saved", dump_ok)
check("power screen: SHUTDOWN row rendered", len(bands) == 4 and bands[0] > 20)
check("power screen: RESTART item visible", len(bands) == 4 and bands[1] > 20)
check("power screen: SLEEP NOW row rendered", len(bands) == 4 and bands[2] > 20)
check("power screen: BACK row rendered", len(bands) == 4 and bands[3] > 20)

p.kill()
p.wait()

# ----------------------------------------------------------------
# Phase D (LAST, separate boot): poweroff terminates QEMU.
p = boot_qemu()
ser2 = conn(SER)
ser2.settimeout(0.3)
serial = b""
start_reader(ser2)
ok_prompt3 = wait_from(0, "qaon>", 150)
check("poweroff phase: boot reaches qaon> prompt", ok_prompt3)
ser2.sendall(b"poweroff\n")
t0 = time.time()
exited = False
while time.time() - t0 < 30:
    if p.poll() is not None:
        exited = True
        break
    time.sleep(0.3)
check("poweroff: QEMU exits on its own", exited)
check("poweroff: QEMU exit code 0", exited and p.poll() == 0)
check("poweroff: kernel logged SYS_POWEROFF",
      "sys: SYS_POWEROFF (software request)" in txt())
if not exited:
    p.kill()
p.wait()

fails = [n for n, ok in results if not ok]
print("FND1-VERIFY:", "PASS" if not fails else f"FAIL {fails}")
sys.exit(0 if not fails else 1)
