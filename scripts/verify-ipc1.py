#!/usr/bin/env python3
"""IPC1-VERIFY: poll-only inline Mach IPC (IPC-1).

Drives the two embed-hook test programs in ONE dev-mode
boot (typed serial only -- no `gui`, so sh owns the
console and typed commands are safe, the FND-4 pattern):

  run embed:uipcsrv   setup: alloc its port (qlimit 16),
                      park the receive right in the
                      kernel test rendezvous.
  run embed:uipccli   setup + negative probes, then
                      send batch 0 (16 requests) and park
                      its reply port.
  ... alternating drain runs: the server claims its
  port, receives one batch, verifies seq/payload,
  replies on each header send-once right; the client
  claims its reply port, verifies the replies, sends
  the next batch. Batch = 16 because upstream Mach caps
  a port queue at MACH_PORT_QLIMIT_MAX = 16; 100
  messages = 7 batches (16*6 + 4). Coordination state
  lives in ramfs counter files (the house pattern).

Expected evidence (guest behaviour, not code):
  - client probes: empty queue -> -5 (UIPC_EEMPTY),
    full queue -> -4 (UIPC_EFULL) after exactly the
    qlimit, dead port -> -3 (UIPC_EDEST), oversize body
    (4097 B) -> -6 (UIPC_E2BIG); no hang, no fault.
  - "UIPCSRV: PASS served=100/100" and
    "UIPCCLI: PASS replies=100/100": 100 sequenced
    messages round-tripped with byte-exact payloads,
    including a 4 B body (seq 0) and a body at the
    4096 B inline cap (seq 50).
  - every run exits 0; the shell stays responsive and
    fail_count stays 0 (no kernel fault anywhere).

Pre-change shape: syscalls 10/11/25 are the dispatch's
-2 (ENOSYS) default, so both programs print
"FAIL claim rc=-2" and exit 1 -- this script FAILs.

Harness rules: the serial socket is drained
continuously by a reader thread (guest printf is
polling-mode); leftover qemu-system-arm is killed by
exact name first. IPC state is RAM-only, so nothing on
the dev disk needs scrubbing or restoring.
"""
import os, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"

subprocess.run(["pkill", "-x", "qemu-system-arm"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)

SER = "/tmp/ipc1-ser.sock"
MON = "/tmp/ipc1-mon.sock"
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


p = subprocess.Popen([
    "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
    "-L", tc + "/usr/share/qemu",
    "-kernel", ROOT + "/mach3-port/build/mach3.elf",
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

ser = conn(SER)
ser.settimeout(0.3)
start_reader(ser)
mon = conn(MON)
mon.settimeout(5)
try:
    mon.recv(4096)
except Exception:
    pass


def shell_cmd(cmd, wait_for, timeout=30):
    pos = len(serial)
    ser.sendall(cmd.encode() + b"\n")
    return wait_from(pos, wait_for, timeout)


def run_prog(name, timeout=60):
    """`run embed:<name>`; returns the serial segment."""
    pos = len(serial)
    ser.sendall(f"run embed:{name}\n".encode())
    wait_from(pos, "exit ", timeout)
    time.sleep(0.3)
    return txt()[pos:]


check("boot reaches qaon> prompt", wait_from(0, "qaon>", 150))
check("shell alive (bootctl)", shell_cmd("bootctl", "fail_count=", 30))

srv_pass = False
cli_pass = False
seg = run_prog("uipcsrv")
check("srv setup run exits 0", "exit 0\n" in seg)
check("srv setup: port published via rendezvous",
      "UIPCSRV: setup port published" in seg)

seg = run_prog("uipccli")
check("cli setup run exits 0", "exit 0\n" in seg)
check("probe: recv on empty queue -> -5 (EEMPTY)",
      "UIPCCLI: probe empty-queue rc=-5 OK" in seg)
check("probe: send to full queue -> -4 (EFULL)",
      "UIPCCLI: probe full-queue rc=-4 OK" in seg)
check("probe: send to dead port -> -3 (EDEST)",
      "UIPCCLI: probe dead-port rc=-3 OK" in seg)
check("probe: oversize body 4097 -> -6 (E2BIG)",
      "UIPCCLI: probe oversize rc=-6 OK" in seg)
check("cli setup: batch 0 sent (16 requests queued)",
      "UIPCCLI: progress sent=16 got=0" in seg)

# Alternating drain rounds: srv serves one batch, cli
# verifies its replies and sends the next batch.
for rnd in range(10):
    if srv_pass and cli_pass:
        break
    if not srv_pass:
        seg = run_prog("uipcsrv")
        check(f"srv drain round {rnd} exits 0", "exit 0\n" in seg)
        if "UIPCSRV: PASS served=100/100" in seg:
            srv_pass = True
    if not cli_pass:
        seg = run_prog("uipccli")
        check(f"cli round {rnd} exits 0", "exit 0\n" in seg)
        if "UIPCCLI: PASS replies=100/100" in seg:
            cli_pass = True

check("server served 100/100 sequenced requests", srv_pass)
check("client verified 100/100 replies (order+payload)", cli_pass)
check("no FAIL marker from either test program",
      "UIPCSRV: FAIL" not in txt() and "UIPCCLI: FAIL" not in txt())
check("no kernel fault during the whole run",
      "FAULT" not in txt())
check("shell responsive at the end (fail_count=0)",
      shell_cmd("bootctl", "fail_count=0", 30))

p.kill()
with open(os.path.join(ROOT, "logs", "ipc1-serial.log"), "wb") as fh:
    fh.write(serial)
fails = [n for n, ok in results if not ok]
print(f"IPC1-VERIFY: {'PASS' if not fails else 'FAIL'} "
      f"({len(results) - len(fails)}/{len(results)} checks)")
if fails:
    print("failed:", fails)
sys.exit(1 if fails else 0)
