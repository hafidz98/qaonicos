#!/usr/bin/env python3
"""C6L3-VERIFY: v2 completion -- TOOL.DISPATCH, EVT, NET.WS (C6L-3).

The guest program `ulink3` is spawn-only (the bridge leg
needs the scheduler active so yields pump net_pump, per
the C6L-2 finding). This script boots QEMU, waits for
the qaon> prompt, sends `run embed:ulink3` on the serial
console, and the spawned child runs the same checks
twice: once against the in-process mock backend, once
against the host peer over the bridge. Per leg: a
TEST.SCRIPT sequence of peer-initiated dispatches
(get_info ALLOW, gpio_read ALLOW, gpio_write CONFIRM,
self_destruct BLOCK) plus an EVT WIFI.DROPPED; a
corrupt-CRC EVT frame that must report a link error
and recover after reopen; a NET.WS session (pre-OPEN
reject, OPEN sid 1, second OPEN BUSY, echo, bad-CRC
BIN reject, CLOSE, second CLOSE NOCONN);
guest-originated TOOL.DISPATCH (get_info, gpio_read x2,
fail_test JSON failure, unknown tool NOTOOL); and a
final TIME.GET proving the stream stayed in sync.

Pre-change shape: on the C6L-2 kernel there is no
`embed:ulink3` spawn hook, so the shell answers
`spawn gagal` and no ULINK3 line ever appears --
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
TRANSCRIPT = "/tmp/c6l3-transcript.log"
STATS = "/tmp/c6l3-stats.json"

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


SER = "/tmp/c6l3-ser.sock"
MON = "/tmp/c6l3-mon.sock"
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


MOCK_MARKERS = [
    "mock script armed",
    "mock dispatch get_info",
    "mock dispatch gpio_read",
    "mock dispatch gpio_write confirm",
    "mock dispatch self_destruct refused",
    "mock evt dropped",
    "mock dispatch receipts",
    "mock bad evt reset",
    "mock sync after reset",
    "mock ws preopen rejected",
    "mock ws open",
    "mock ws busy",
    "mock ws echo",
    "mock ws bad crc rejected",
    "mock ws close",
    "mock ws noconn",
    "mock tool get_info",
    "mock tool gpio_read",
    "mock tool fail",
    "mock tool notool",
    "mock sync time",
]

BRIDGE_MARKERS = [
    "bridge script armed",
    "bridge dispatch get_info",
    "bridge dispatch gpio_read",
    "bridge dispatch gpio_write confirm",
    "bridge dispatch self_destruct refused",
    "bridge evt dropped",
    "bridge bad evt reset",
    "bridge sync after reset",
    "bridge ws preopen sent",
    "bridge ws preopen dropped",
    "bridge ws open",
    "bridge ws busy",
    "bridge ws echo",
    "bridge ws bad crc dropped",
    "bridge ws close",
    "bridge ws noconn",
    "bridge tool get_info",
    "bridge tool gpio_read",
    "bridge tool fail",
    "bridge tool notool",
    "bridge sync time",
]

peer = None
qemu = None
try:
    peer = start_peer()
    qemu, ser, mon = launch_qemu()

    # ---- Boot first: v1.1 + voice regressions, then spawn ulink3 ----
    check("boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    check("mock regression ULINK: PASS",
          "ULINK: PASS" in txt())
    check("UVOICE regression marker present", "UVOICE: PASS" in txt())
    pos = len(serial)
    ser.sendall(b"run embed:ulink3\n")
    check("spawn accepted (no 'spawn gagal')",
          wait_from(pos, "ulink3: v2 completion", 60))

    check("guest prints ULINK3: PASS",
          wait_from(0, "ULINK3: PASS", 240))
    seg = txt()
    for m in MOCK_MARKERS + BRIDGE_MARKERS:
        check("marker present: ULINK3: " + m + " ok",
              ("ULINK3: " + m + " ok") in seg)
    check("marker present: ULINK3: bridge no drops ok",
          "ULINK3: bridge no drops ok" in seg)
    check("no ULINK3: FAIL line", "ULINK3: FAIL" not in seg)
    check("no ULINK3: SKIP (peer was running)",
          "ULINK3: SKIP" not in seg)
    check("shell reports child exit 0", "exit 0\n" in seg)
    check("no kernel fault anywhere", "FAULT" not in seg)

    # ---- Peer-side evidence (bridge leg only) ----
    with open(STATS) as f:
        st = json.load(f)
    check("peer stats: 5 guest-originated dispatches",
          st.get("tool_dispatch") == 5)
    check("peer stats: 4 dispatch results received",
          st.get("dispatch_results") == 4)
    check("peer stats: 1 WS echo served",
          st.get("ws_echo") == 1)
    check("peer stats: 2 WS drops (preopen + bad CRC)",
          st.get("ws_drop") == 2)
    with open(TRANSCRIPT) as f:
        tr = f.read()
    check("transcript shows scripted DISPATCH get_info sent",
          "SEND" in tr and "DISPATCH 1 get_info" in tr)
    check("transcript shows scripted DISPATCH gpio_read sent",
          "DISPATCH 2 gpio_read" in tr)
    check("transcript shows scripted DISPATCH gpio_write sent",
          "DISPATCH 3 gpio_write" in tr)
    check("transcript shows scripted DISPATCH self_destruct sent",
          "DISPATCH 4 self_destruct" in tr)
    check("transcript shows scripted EVT sent",
          "WIFI.DROPPED" in tr)
    check("transcript shows get_info result with uptime_ms",
          re.search(r"RESULT 1 .*uptime_ms", tr) is not None)
    check("transcript shows gpio_read result pin 3 value 1",
          re.search(r"RESULT 2 .*\"pin\":3.*\"value\":1", tr)
          is not None)
    check("transcript shows needs_confirm result",
          re.search(r"RESULT 3 .*needs_confirm", tr) is not None)
    check("transcript shows refused result",
          re.search(r"RESULT 4 .*refused", tr) is not None)
    check("transcript shows guest TOOL.DISPATCH get_info",
          "TOOL.DISPATCH get_info" in tr)
    check("transcript shows guest TOOL.DISPATCH gpio_read",
          "TOOL.DISPATCH gpio_read" in tr)
    check("transcript shows guest TOOL.DISPATCH fail_test",
          "TOOL.DISPATCH fail_test" in tr)
    check("transcript shows guest TOOL.DISPATCH nope_tool",
          "TOOL.DISPATCH nope_tool" in tr)
    check("transcript shows NOTOOL response",
          "ERR NOTOOL" in tr)
    check("transcript shows WS echo frame",
          "FRAME BIN" in tr and "echo" in tr)
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
    left = subprocess.run(["pgrep", "-f", "mock-c6.py"],
                          capture_output=True, text=True).stdout.strip()
    if left != "":
        time.sleep(1.0)
        left = subprocess.run(["pgrep", "-f", "mock-c6.py"],
                              capture_output=True, text=True).stdout.strip()
    check("no mock-c6.py left running", left == "")

npass = sum(1 for _, c in results if c)
print(f"C6L3-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
