#!/usr/bin/env python3
"""C6L1-VERIFY: link layer + wire freeze probe (C6L-1).

Boot 1 (dev mode, worktree kernel): the boot one-shot
`ulinktest` runs before the shell prompt and prints one
marker line per check plus ULINK: PASS/FAIL. This script
asserts every marker, the UVOICE regression marker, and a
clean boot 2 (ULINK runs again at every boot; fail_count
must be 0, proving ulinktest's NVS cleanup left no damage).

Pre-change shape: on the IPC-5 merge kernel (no
ulinktest, no link layer) no ULINK line ever appears --
this script FAILs. Set KERNEL to that ELF to reproduce.

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

SER = "/tmp/c6l1-ser.sock"
MON = "/tmp/c6l1-mon.sock"
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


MARKERS = [
    "ULINK: wifi scan ok",
    "ULINK: wifi connect needpass ok",
    "ULINK: wifi pass connected ok",
    "ULINK: wifi open connected ok",
    "ULINK: wifi err notfound ok",
    "ULINK: wifi err auth ok",
    "ULINK: wifi err noconn ok",
    "ULINK: wifi list forget ok",
    "ULINK: wifi status disc ok",
    "ULINK: ble status spec ok",
    "ULINK: ble on off name ok",
    "ULINK: kv len-prefixed ok",
    "ULINK: kv errors ok",
    "ULINK: kv trunc ok",
    "ULINK: kv legacy rejected ok",
    "ULINK: aud qa frame ok",
    "ULINK: legacy aud rejected ok",
    "ULINK: stub backend swapped ok",
    "ULINK: time fido unknown ok",
    "ULINK: malformed kv resync ok",
]

# ================= Boot 1 =================
check("boot 1 prints ULINK: PASS", wait_from(0, "ULINK: PASS", 240))
seg = txt()
for m in MARKERS:
    check("marker present: " + m, m in seg)
check("no ULINK: FAIL line", "ULINK: FAIL" not in seg)
check("UVOICE regression marker present", "UVOICE: PASS" in seg)
check("no kernel fault anywhere", "FAULT" not in seg)
check("boot 1 reaches qaon> prompt", wait_from(0, "qaon>", 60))

# ================= Boot 2 =================
nb = len(boot_lines())
pos = len(serial)
ser.sendall(b"reboot\n")
t0 = time.time()
while len(boot_lines()) <= nb and time.time() - t0 < 120:
    time.sleep(0.3)
check("boot 2 arrives after reboot", len(boot_lines()) > nb)
check("boot 2 prints ULINK: PASS again", wait_from(pos, "ULINK: PASS", 240))
check("boot 2 reaches qaon> prompt", wait_from(pos, "qaon>", 60))
check("boot 2 shell alive (fail_count=0)",
      shell_cmd("bootctl", "fail_count=0", 30))

npass = sum(1 for _, c in results if c)
print(f"C6L1-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
try:
    mon.sendall(b"quit\n")
except Exception:
    pass
time.sleep(0.5)
try:
    p.wait(timeout=10)
except Exception:
    p.kill()
sys.exit(0 if npass == len(results) else 1)
