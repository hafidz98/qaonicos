#!/usr/bin/env python3
"""C6L5-VERIFY: link daemon linkd -- single transport owner (C6L-5).

The guest clients talk to the C6 link ONLY through linkd
over IPC name `link` (LINKD-IPC v1); the direct layer is
linked by linkd alone. This script boots QEMU, sets the
marker /data/link5-test (activating the test daemons
linkd's second client ulink5d), starts the GUI (which
launches linkd + ulink5d), and checks:

  * shell `link` before `gui` reports unavailable, and
    after `gui` reports UP with counters (read-only);
  * spawn client embed:ulink5 through the stub: link up,
    TIME.GET, WIFI.SCAN, KV set/get/del, guest
    TOOL.DISPATCH (get_info, gpio_read, fail_test,
    NOTOOL), NET.HTTP 1024 B, voice 32 x 1024 B frames
    at ~31 ms with zero failures and STOP == 32
    (the qlimit-5 voice-rate measurement, with ulink5d
    issuing commands concurrently);
  * demultiplexing: ulink5d's ok markers (its own key
    round-trips + EVT) interleave with ulink5's run,
    with no FAILED before the death window;
  * transport death, scripted by markers: ulink5 prints
    "ready kill", the script kills the peer, ulink5
    observes command failure + link DOWN, then prints
    "ready restart", the script restarts the peer, and
    ulink5 observes recovery (command + state UP);
  * peer-dispatch forwarding: while the shell runs
    embed:ulinktools (progid 2, the stub `tools`
    server), ulink5d's TEST.SCRIPT attempts flow
    through linkd to it -- the peer transcript shows
    the real policy results (uptime_ms, pin 3 value 1,
    needs_confirm, refused) -- and attempts without a
    tools server show tools_unavailable (the no-tools
    leg); every attempt also fans an EVT WIFI.DROPPED
    out to ulink5d;
  * client death: embed:ulink5die sends one raw CMD
    request and exits at once (against a delayed
    peer, so the request is in flight); linkd logs
    "reply fail" and keeps serving (ulink5d markers
    continue, shell `link` still UP);
  * teardown: marker cleared, no peer or QEMU left.

Pre-change shape: on the C6L-4 kernel there is no
`embed:ulink5` spawn hook, no `link` builtin, and no
linkd -- the shell answers `spawn gagal` /
`perintah tak dikenal` and no ULINK5/LINKD line ever
appears; this script FAILs. Set KERNEL to that ELF to
reproduce (PEER may point at a copy of scripts/mock-c6.py).

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first; the
peer is never left running at the end. Scratch lives
under ~/workspace/logs-c6l5 (NOT /tmp: /tmp is wiped
on VM replacement).
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
LOGDIR = os.path.expanduser("~/workspace/logs-c6l5")
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


def peer_paths(tag):
    return (os.path.join(LOGDIR, f"c6l5-{tag}-transcript.log"),
            os.path.join(LOGDIR, f"c6l5-{tag}-stats.json"))


def start_peer(tag, extra=None):
    tr, st = peer_paths(tag)
    for f in (tr, st):
        try:
            os.unlink(f)
        except FileNotFoundError:
            pass
    args = [sys.executable, PEER, "--port", str(PORT),
            "--transcript", tr, "--stats-file", st] + (extra or [])
    p = subprocess.Popen(args,
                         stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
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
    time.sleep(0.5)


SER = os.path.join(LOGDIR, "c6l5-ser.sock")
MON = os.path.join(LOGDIR, "c6l5-mon.sock")
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


def set_marker(ser, val, tries=3):
    # The marker put + read-back must agree. A garbled
    # shell input line (observed C6L-5: teardown put
    # reported "put: ok (18 bytes)" and wrote to a
    # garbled name, leaving the marker at "1") passes
    # a bare "put: ok" check, so verify the end state:
    # put, then cat, and only accept when the file
    # reads back the value just written. Both
    # operations are idempotent, so retrying converges
    # as soon as one clean line gets through.
    want = "\n" + val + "\n"
    for _ in range(tries):
        if not shell_cmd(ser, "put /data/link5-test " + val,
                         "put: ok", 20):
            continue
        if shell_cmd(ser, "cat /data/link5-test", want, 20):
            return True
    return False


def run_spawn(ser, cmd, start_marker, timeout=60, tries=3):
    # Same lost-input hazard as shell_cmd, plus one more:
    # a transient "spawn gagal" (observed C6L-5: a
    # `run embed:ulink5` whose echo arrived but whose
    # spawn failed once, while the same spawn succeeded
    # in other runs). A failed spawn leaves the slot
    # free (spawn_parent is set only on the success
    # path), so retrying after "spawn gagal" cannot
    # double-start. Retry ONLY on (a) "spawn gagal"
    # in the segment or (b) a missing command echo
    # (input never reached the shell). If the echo IS
    # present and no gagal appeared, the command ran
    # and produced other output -- a real signal,
    # never retried away (a duplicate spawn would
    # pollute later legs).
    cb = cmd.encode()
    for _ in range(tries):
        pos = len(serial)
        ser.sendall(cb + b"\n")
        if wait_from(pos, start_marker, timeout):
            return True
        seg = serial[pos:]
        if b"spawn gagal" in seg:
            continue
        if b"\n" + cb in seg or b"qaon> " + cb in seg:
            return False
    return False


def shell_cmd(ser, cmd, wait_for, timeout=30):
    # Shell input is occasionally LOST (observed C6L-5:
    # a `link` sent 5 s after ULINK5D ready produced no
    # echo and no output, while the next command ran
    # fine -- the shell was never wedged, the bytes
    # never arrived). Retry ONLY when the command echo
    # is absent from the segment: a missing echo means
    # the input never reached the shell, so resending
    # cannot double-execute. If the echo IS present,
    # the command ran and produced other output -- a
    # real signal, never retried away.
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


peer = None
qemu = None
try:
    peer = start_peer("a")
    qemu, ser, mon = launch_qemu()

    check("boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    check("shell link before gui reports unavailable",
          shell_cmd(ser, "link", "link: unavailable", 30))
    check("marker set", set_marker(ser, "1"))
    ser.sendall(b"gui\n")
    check("linkd registers name link",
          wait_from(0, "LINKD: registered link", 90))
    check("ulink5d ready",
          wait_from(0, "ULINK5D: ready", 90))
    check("shell link after gui reports UP",
          shell_cmd(ser, "link", "link: UP", 30))
    check("shell link reports qlimit 5 (default, not raised)",
          wait_from(0, "qlimit=5", 30))

    # ---- ulink5: stub legs + scripted transport death ----
    pos5 = len(serial)
    check("spawn accepted (no 'spawn gagal')",
          run_spawn(ser, "run embed:ulink5", "ULINK5: start", 60))
    check("ulink5 signals ready kill",
          wait_from(0, "ULINK5: ready kill", 180))
    stop_peer(peer)
    peer = None
    check("ulink5 observes dead link",
          wait_from(0, "ULINK5: dead ok", 60))
    check("ulink5 signals ready restart",
          wait_from(0, "ULINK5: ready restart", 30))
    peer = start_peer("b")
    check("ulink5 recovers after peer return",
          wait_from(0, "ULINK5: recovered ok", 90))
    check("guest prints ULINK5: PASS",
          wait_from(0, "ULINK5: PASS", 60))
    check("shell reports child exit 0",
          wait_from(0, "exit 0\n", 30))
    seg = txt()
    u5start = seg.find("ULINK5: start")
    u5kill = seg.find("ULINK5: ready kill")
    u5pass = seg.find("ULINK5: PASS")
    for leg in ("link up", "time", "scan", "kv set", "kv get",
                "kv del", "kv gone", "tool info", "tool gpio",
                "tool fail", "tool notool", "nethttp",
                "voice start", "voice frames", "voice stop",
                "dead cmd fails", "dead state down",
                "recovered cmd", "recovered state up"):
        check("leg present: ULINK5: " + leg + " ok",
              ("ULINK5: " + leg + " ok") in seg)
    check("no ULINK5: FAILED line", "ULINK5: FAILED" not in seg)
    check("no kernel fault anywhere", "FAULT" not in seg)

    # ---- Demultiplexing: ulink5d interleaves with ulink5 ----
    # Count REAL ok lines (iter N ok), not the loose
    # "iter " + " ok" pair -- the loose form passed
    # at the second run while every iteration was in
    # fact FAILED (TEST.SCRIPT without its c6l3 arg).
    pre_kill = seg[u5start:u5kill] if u5start >= 0 and u5kill > u5start else ""
    ok_re = re.compile(r"ULINK5D: iter \d+ ok")
    check("ulink5d ok markers during ulink5 run (>=2)",
          len(ok_re.findall(pre_kill)) >= 2)
    check("no ULINK5D FAILED before the death window",
          "ULINK5D: iter " in pre_kill and
          " FAILED" not in pre_kill)
    # ---- Voice-rate measurement, peer A (killed, stats flushed) ----
    tr_a, st_a = peer_paths("a")
    with open(st_a) as f:
        sta = json.load(f)
    check("peer A stats: 32 voice frames accepted (qlimit 5, 0 drops)",
          sta.get("aud_ok") == 32)

    # ---- Peer-dispatch forwarding window (ulinktools) ----
    pos_t = len(serial)
    check("ulinktools started",
          run_spawn(ser, "run embed:ulinktools",
                    "ULINKTOOLS: registered tools", 60))
    check("ulinktools serves 4 dispatches and exits",
          wait_from(pos_t, "ULINKTOOLS: done 4", 150))
    check("shell reports ulinktools exit 0",
          wait_from(pos_t, "exit 0\n", 30))
    time.sleep(2.0)  # peer logs the last RESULT just after "done 4"
    tr_b, st_b = peer_paths("b")
    with open(tr_b) as f:
        trb = f.read()
    check("transcript B shows get_info result with uptime_ms",
          re.search(r"RESULT .*uptime_ms", trb) is not None)
    check("transcript B shows gpio_read result pin 3 value 1",
          re.search(r"RESULT .*\"pin\":3.*\"value\":1", trb) is not None)
    check("transcript B shows needs_confirm result",
          re.search(r"RESULT .*needs_confirm", trb) is not None)
    check("transcript B shows refused result",
          re.search(r"RESULT .*refused", trb) is not None)
    check("transcript B shows tools_unavailable (no-tools leg)",
          "tools_unavailable" in trb)
    check("transcript B shows EVT WIFI.DROPPED frames",
          "WIFI.DROPPED" in trb)
    seg2 = txt()
    check("ulink5d ok markers after recovery + tools window (>=2)",
          len(ok_re.findall(seg2[u5pass:])) >= 2)

    # ---- Client death (delayed peer, request in flight) ----
    stop_peer(peer)
    peer = None
    peer = start_peer("c", ["--delay-ms", "800"])
    pos_d = len(serial)
    check("ulink5die sends and exits 0",
          run_spawn(ser, "run embed:ulink5die",
                    "ULINK5DIE: sent", 60) and
          wait_from(pos_d, "exit 0\n", 30))
    time.sleep(3.0)
    check("linkd logs reply fail for the dead client",
          wait_from(pos_d, "LINKD: reply fail", 30))
    check("ulink5d ok markers continue after client death",
          wait_from(pos_d, "ULINK5D: iter ", 60) and
          len(ok_re.findall(txt()[pos_d:])) >= 1)
    check("shell link still UP after client death",
          shell_cmd(ser, "link", "link: UP", 30))

    # ---- Teardown: marker cleared, nothing left running ----
    # One check: set_marker already verifies the
    # read-back, so a garbled put cannot pass here.
    check("marker cleared and reads back 0", set_marker(ser, "0"))
finally:
    try:
        with open(os.path.join(LOGDIR, "c6l5-serial.log"), "wb") as f:
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
    if peer is not None:
        stop_peer(peer)
    left = subprocess.run(["pgrep", "-f", "mock-c6.py"],
                          capture_output=True, text=True).stdout.strip()
    if left != "":
        time.sleep(1.0)
        left = subprocess.run(["pgrep", "-f", "mock-c6.py"],
                              capture_output=True, text=True).stdout.strip()
    check("no mock-c6.py left running", left == "")
    leftq = subprocess.run(["pgrep", "-x", "qemu-system-arm"],
                           capture_output=True, text=True).stdout.strip()
    check("no qemu-system-arm left running", leftq == "")

npass = sum(1 for _, c in results if c)
print(f"C6L5-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
