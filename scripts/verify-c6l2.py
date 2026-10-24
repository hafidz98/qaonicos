#!/usr/bin/env python3
"""C6L2-VERIFY: host-bridge backend + Python mock C6 peer (C6L-2).

The guest program `ulinkbr` is spawn-only (a boot
one-shot runs with the scheduler inactive, where
SYS_YIELD is a nop and net_pump never runs, so the
bridge could never complete a handshake there). This
script boots QEMU, waits for the qaon> prompt, sends
`run embed:ulinkbr` on the serial console, and the
spawned child runs the frozen v1.1 command set
against the host peer, plus AUD frames, a NET.HTTP
hash check, and a peer-death + reconnect sequence:
when the guest announces PHASE-A-DONE the script
kills and restarts the peer, then waits for PASS.

Pre-change shape: on the C6L-1 kernel there is no
`embed:ulinkbr` spawn hook, so the shell answers
`spawn gagal` and no ULINKBR line ever appears --
this script FAILs. Set KERNEL to that ELF to reproduce
(PEER may point at a copy of scripts/mock-c6.py).

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first; the
peer is never left running at the end.
"""
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL = os.environ.get("KERNEL", ROOT + "/mach3-port/build/mach3.elf")
PEER = os.environ.get("PEER", os.path.join(ROOT, "scripts", "mock-c6.py"))
PORT = 18082
TRANSCRIPT = "/tmp/c6l2-transcript.log"
STATS = "/tmp/c6l2-stats.json"

tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"

subprocess.run(["pkill", "-x", "qemu-system-arm"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)

for f in (TRANSCRIPT, STATS):
    try:
        os.unlink(f)
    except FileNotFoundError:
        pass

results = []


def check(name, cond):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name, flush=True)


def start_peer():
    p = subprocess.Popen(
        [sys.executable, PEER, "--port", str(PORT),
         "--transcript", TRANSCRIPT, "--stats-file", STATS],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < 15:
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=1)
            s.close()
            return p
        except OSError:
            if p.poll() is not None:
                break
            time.sleep(0.2)
    raise RuntimeError("peer did not start listening")


def stop_peer(p):
    try:
        p.terminate()
        p.wait(timeout=5)
    except Exception:
        try:
            p.kill()
        except Exception:
            pass


SER = "/tmp/c6l2-ser.sock"
MON = "/tmp/c6l2-mon.sock"
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


peer = None
qemu = None
try:
    peer = start_peer()
    qemu, ser, mon = launch_qemu()

    # ---- Boot first: mock regression, then spawn ulinkbr ----
    check("boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    check("mock regression ULINK: PASS",
          "ULINK: PASS" in txt())
    check("UVOICE regression marker present", "UVOICE: PASS" in txt())
    pos = len(serial)
    ser.sendall(b"run embed:ulinkbr\n")
    check("spawn accepted (no 'spawn gagal')",
          wait_from(pos, "ulinkbr: bridge backend", 60))

    # ---- Phase A: guest runs against the live peer ----
    check("guest announces PHASE-A-DONE",
          wait_from(0, "ULINKBR: PHASE-A-DONE", 240))

    # ---- Peer-side evidence, before the restart wipes stats ----
    with open(STATS) as f:
        st = json.load(f)
    check("peer stats: 8 AUD frames accepted",
          st.get("aud_ok") == 8)
    check("peer stats: 2 rejected frames (legacy + bad CRC)",
          st.get("aud_bad") == 2)
    check("peer stats: NET.HTTP served 1024 bytes",
          st.get("nethttp_bytes") == 1024)

    # ---- Peer death + restart, mid-session ----
    stop_peer(peer)
    time.sleep(1.0)
    peer = start_peer()

    # ---- Guest must finish: death detected, reconnect ok ----
    check("guest prints ULINKBR: PASS",
          wait_from(0, "ULINKBR: PASS", 180))
    seg = txt()
    for m in [
        "ULINKBR: wifi scan ok",
        "ULINKBR: wifi connect needpass ok",
        "ULINKBR: wifi pass connected ok",
        "ULINKBR: wifi open connected ok",
        "ULINKBR: wifi err notfound ok",
        "ULINKBR: wifi err auth ok",
        "ULINKBR: wifi err noconn ok",
        "ULINKBR: wifi list forget ok",
        "ULINKBR: wifi status disc ok",
        "ULINKBR: ble status spec ok",
        "ULINKBR: ble on off name ok",
        "ULINKBR: kv len-prefixed ok",
        "ULINKBR: kv errors ok",
        "ULINKBR: kv trunc ok",
        "ULINKBR: kv legacy rejected ok",
        "ULINKBR: aud frames ok",
        "ULINKBR: nethttp hash ok",
        "ULINKBR: peer death detected ok",
        "ULINKBR: peer reconnect ok",
    ]:
        check("marker present: " + m, m in seg)
    check("no ULINKBR: FAIL line", "ULINKBR: FAIL" not in seg)
    check("no ULINKBR: SKIP (peer was running)",
          "ULINKBR: SKIP" not in seg)
    check("shell reports child exit 0", "exit 0\n" in seg)
    check("no kernel fault anywhere", "FAULT" not in seg)

    # ---- Peer-side evidence ----
    with open(TRANSCRIPT) as f:
        tr = f.read()
    check("transcript shows exact KV.SET wire bytes",
          "b'KV.SET ulinkbr_wire 3\\nxyz'" in tr)
    check("transcript shows two peer sessions (reconnect)",
          tr.count("ACCEPT") >= 2)
    check("transcript shows AUD frames accepted",
          "FRAME AUD 512 ok" in tr)
finally:
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
    if peer is not None:
        stop_peer(peer)
    # Grace: a terminated peer may need a moment to
    # exit before the pgrep check observes it gone.
    left = subprocess.run(["pgrep", "-f", "mock-c6.py"],
                          capture_output=True, text=True).stdout.strip()
    if left != "":
        time.sleep(1.0)
        left = subprocess.run(["pgrep", "-f", "mock-c6.py"],
                              capture_output=True, text=True).stdout.strip()
    check("no mock-c6.py left running", left == "")

npass = sum(1 for _, c in results if c)
print(f"C6L2-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
