#!/usr/bin/env python3
"""IPC5-VERIFY: AUD-rate queue sizing + close-out probe (IPC-5).

Boot 1 (dev mode, typed serial -- sh owns the console):
  run embed:uipcaud, a single spawn child that measures
  the frozen uipc.h v1 contract at the C6L voice rate
  (16 kHz x 16 bit = 32 KB/s):
    A: stall-fill, qlimit 16, 4096 B frames, paced 8/s.
       Expect fill=16 (the Mach ceiling), drained=16,
       elapsed ~2 s (16 slots x 125 ms).
    B: stall-fill at the DEFAULT qlimit (no SETQLIMIT),
       1024 B frames, paced ~31/s. Expect fill=5 --
       the default is proven by count, not asserted.
    C: sustained loopback, 4096 B x 240 frames over
       ~30 s at 8/s, send + blocking recv per slot.
       Expect recv=240, drops=0.
    D: 1 KB round-trip cost, 200 x send + poll recv
       unpaced. Expect completion; the ms number is
       tick-quantized (10 ms) and reported as such.
    E: deadline-park cadence, 100 x park(10 ms) on an
       empty port. Expect etime=100, ~100 iters/s --
       the cadence the fan-in poll-loop pattern relies
       on (see DEVLOG-IPC5).
  Boot 2 (same disk): shell alive, fail_count=0.

Pre-change shape: the uipcaud program and its embed
hook do not exist on the IPC-4 merge kernel, so the
`run embed:uipcaud` leg finds no program and no AUD:
lines appear -- this script FAILs. Set KERNEL to the
pre-change ELF to reproduce that run.

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first.
"""
import os, re, socket, subprocess, sys
import threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get("KERNEL", ROOT + "/mach3-port/build/mach3.elf")
tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"

subprocess.run(["pkill", "-x", "qemu-system-arm"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)

SER = "/tmp/ipc5-ser.sock"
MON = "/tmp/ipc5-mon.sock"
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


p, ser, mon = launch_qemu()


def shell_cmd(cmd, wait_for, timeout=30):
    pos = len(serial)
    ser.sendall(cmd.encode() + b"\n")
    return wait_from(pos, wait_for, timeout)


# ================= Boot 1 =================
check("boot 1 reaches qaon> prompt", wait_from(0, "qaon>", 150))
pos = len(serial)
ser.sendall(b"run embed:uipcaud\n")
done = wait_from(pos, "AUD: DONE", 240)
seg = txt()[pos:]
mA = re.search(r"AUD: A fill=(\d+) elapsed_ms=(\d+)", seg)
mAd = re.search(r"AUD: A drained=(\d+)", seg)
mB = re.search(r"AUD: B fill=(\d+) elapsed_ms=(\d+)", seg)
mC = re.search(r"AUD: C sent=(\d+) recv=(\d+) drops=(\d+) "
                r"elapsed_ms=(\d+) rate_x10=(\d+)", seg)
mD = re.search(r"AUD: D iters=(\d+) elapsed_ms=(\d+) "
                r"us_per_roundtrip=(\d+)", seg)
mE = re.search(r"AUD: E parks=(\d+) etime=(\d+) elapsed_ms=(\d+) "
                r"iters_per_s=(\d+)", seg)

check("probe runs to AUD: DONE", done)
check("probe reports no FAIL line", "AUD: FAIL" not in seg)
check("A: stall-fill at qlimit 16 fills exactly 16",
      mA is not None and mA.group(1) == "16")
check("A: fill takes ~2 s at 8/s (1000..5000 ms)",
      mA is not None and 1000 <= int(mA.group(2)) <= 5000)
check("A: all 16 frames drain in order",
      mAd is not None and mAd.group(1) == "16")
check("B: default qlimit proven by count: fill=5",
      mB is not None and mB.group(1) == "5")
check("B: fill takes ~0.16 s at ~31/s (50..1500 ms)",
      mB is not None and 50 <= int(mB.group(2)) <= 1500)
check("C: sustained loopback receives all 240 frames",
      mC is not None and mC.group(2) == "240")
check("C: zero drops over ~30 s at the AUD rate",
      mC is not None and mC.group(3) == "0")
check("C: achieved rate within 6.0..11.0 frames/s",
      mC is not None and 60 <= int(mC.group(5)) <= 110)
check("D: 200 1 KB round trips complete",
      mD is not None and mD.group(1) == "200")
check("D: round-trip loop is fast (elapsed < 5000 ms)",
      mD is not None and int(mD.group(2)) < 5000)
check("E: all 100 deadline parks expire (etime=100)",
      mE is not None and mE.group(2) == "100")
check("E: park cadence ~100/s (80..130 iters/s)",
      mE is not None and 80 <= int(mE.group(4)) <= 130)
check("no kernel fault anywhere", "FAULT" not in txt())
check("shell responsive at the end (fail_count=0)",
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
check("boot 2 shell alive (fail_count=0)",
      shell_cmd("bootctl", "fail_count=0", 30))

npass = sum(1 for _, c in results if c)
print(f"IPC5-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
try:
    mon.sendall(b"quit\n")
except Exception:
    pass
time.sleep(0.5)
try:
    p.terminate()
except Exception:
    pass
sys.exit(0 if npass == len(results) else 1)
