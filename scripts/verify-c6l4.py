#!/usr/bin/env python3
"""C6L4-VERIFY: AUD streaming integrity + error semantics (C6L-4).

The guest program `ulinkaud` is spawn-only (the bridge leg
needs the scheduler active so yields pump net_pump, per
the C6L-2 finding). This script boots QEMU, sends
`run embed:ulinkaud`, and drives the host peer through
one restart per fault phase, keyed on the guest's
READY markers (the guest sleeps 6 s after each READY,
the harness restarts inside that window):

  P1 clean         : 10 s production-paced AUD stream
                     (320 x 1024 B ULS4-tagged frames)
                     + 115200-baud slow-rate demo
  P2 --silent      : retryable TIME.GET times out twice
                     (one retry), LINK DOWN TIMEOUT
  P3 clean         : recovery, LINK UP
  P4 --drop-after 1: truncated-response retry recovery;
                     WIFI.PASS + KV.SET(changed) never
                     retried; NOTFOUND peer ERR never
                     retried (each test on a fresh
                     session: probe = resp 1, target =
                     resp 2, the truncated one -- the
                     peer truncates from response N+1)
  P5 killed        : peer death mid-stream, LINK DOWN,
                     STOP fails
  P6 clean         : reconnect + fresh 32-frame stream
  P7 --junk-after 1: garbage line in the stream ->
                     link reset, resync, next command ok
  P8 clean         : over-long KV.SET line -> ERR ARG
                     (peer ERR, link stays UP);
                     KV.GET __badlen__ -> malformed;
                     KV.GET __hugelen__ -> 2 s timeout;
                     TIME.GET ok after each

Pre-change shape: on the C6L-3 kernel there is no
`embed:ulinkaud` spawn hook, so the shell answers
`spawn gagal` and no ULINKAUD line ever appears --
this script FAILs fast (it skips the phase waits
when the banner never arrives). Set KERNEL to that
ELF to reproduce (PEER may point at a copy of
scripts/mock-c6.py).

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first; the
peer is never left running at the end; each peer
process gets its own transcript/stats files so
per-phase assertions survive the restarts.
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
TAG = os.path.join(ROOT, "logs-c6l4", "c6l4")

tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"

subprocess.run(["pkill", "-x", "qemu-system-arm"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)

for f in os.listdir(os.path.join(ROOT, "logs-c6l4")):
    if f.startswith("c6l4-"):
        try:
            os.unlink(os.path.join(ROOT, "logs-c6l4", f))
        except OSError:
            pass

results = []


def check(name, cond):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name, flush=True)


def peer_files(phase):
    return (f"{TAG}-transcript-{phase}.log",
            f"{TAG}-stats-{phase}.json")


def start_peer(phase, extra):
    tr, st = peer_files(phase)
    p = subprocess.Popen(
        [sys.executable, PEER, "--port", str(PORT),
         "--transcript", tr, "--stats-file", st] + extra,
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
    if p is None:
        return
    try:
        p.terminate()
        p.wait(timeout=5)
    except Exception:
        try:
            p.kill()
        except Exception:
            pass


def restart_peer(cur, phase, extra):
    stop_peer(cur)
    time.sleep(0.5)
    return start_peer(phase, extra)


SER = os.path.join(ROOT, "logs-c6l4", "c6l4-ser.sock")
MON = os.path.join(ROOT, "logs-c6l4", "c6l4-mon.sock")
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


def read_text(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


def read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def count_re(needle, text):
    return len(re.findall(needle, text))


MARKERS = [
    "stream 320",
    "stream stop 320",
    "stream stats",
    "rate 115200 slow",
    "rate stats",
    "silence link down timeout",
    "silence link up",
    "drop retry recovered",
    "pass never retried",
    "pass landed once",
    "set never retried",
    "set landed once",
    "notfound no retry",
    "kill link down",
    "restart stream 32",
    "junk link reset",
    "junk sync",
    "badlen arg",
    "badlen malformed",
    "hugelen timeout",
    "badlen sync",
    "peer err no down",
    "link stats",
]

peer = None
qemu = None
spawned = False
try:
    peer = start_peer("p1", [])
    qemu, ser, mon = launch_qemu()

    check("boot reaches qaon> prompt",
          wait_from(0, "qaon>", 120))
    check("mock regression ULINK: PASS",
          "ULINK: PASS" in txt())
    check("UVOICE regression marker present", "UVOICE: PASS" in txt())
    pos = len(serial)
    ser.sendall(b"run embed:ulinkaud\n")
    spawned = wait_from(pos, "ulinkaud: AUD streaming", 60)
    check("spawn accepted (no 'spawn gagal')", spawned)

    if spawned:
        check("guest reaches READY-SILENCE",
              wait_from(0, "ULINKAUD: READY-SILENCE", 240))
        peer = restart_peer(peer, "p2", ["--silent"])
        check("guest reaches READY-CLEAN",
              wait_from(0, "ULINKAUD: READY-CLEAN", 120))
        peer = restart_peer(peer, "p3", [])
        check("guest reaches READY-DROP",
              wait_from(0, "ULINKAUD: READY-DROP", 120))
        peer = restart_peer(peer, "p4", ["--drop-after", "1"])
        check("guest reaches READY-KILL",
              wait_from(0, "ULINKAUD: READY-KILL", 180))
        stop_peer(peer)
        peer = None
        check("guest reaches READY-RESTART",
              wait_from(0, "ULINKAUD: READY-RESTART", 120))
        peer = start_peer("p6", [])
        check("guest reaches READY-JUNK",
              wait_from(0, "ULINKAUD: READY-JUNK", 120))
        peer = restart_peer(peer, "p7", ["--junk-after", "1"])
        check("guest reaches READY-BADLEN",
              wait_from(0, "ULINKAUD: READY-BADLEN", 120))
        peer = restart_peer(peer, "p8", [])
        check("guest prints ULINKAUD: PASS",
              wait_from(0, "ULINKAUD: PASS", 180))

    seg = txt()
    with open(os.path.join(ROOT, "logs-c6l4", "serial.log"), "w") as f:
        f.write(seg)
    for m in MARKERS:
        check("marker present: ULINKAUD: " + m + " ok",
              ("ULINKAUD: " + m + " ok") in seg)
    check("stream stats line carries sent 320 stop 320",
          "sent 320 stop 320" in seg)
    check("no ULINKAUD: FAIL line", "ULINKAUD: FAIL" not in seg)
    check("no ULINKAUD: SKIP (peer was running)",
          "ULINKAUD: SKIP" not in seg)
    check("shell reports child exit 0", "exit 0\n" in seg)
    check("no kernel fault anywhere", "FAULT" not in seg)

    # ---- Peer-side evidence, per phase ----
    st1 = read_json(peer_files("p1")[1])
    check("P1 peer: 352 AUD ok (320 stream + 32 rate demo)",
          st1.get("aud_ok") == 352)
    check("P1 peer: 0 bad AUD", st1.get("aud_bad") == 0)
    check("P1 peer: 0 seq gaps", st1.get("aud_seq_gap") == 0)
    check("P1 peer: 0 outside-session AUD",
          st1.get("aud_outside") == 0)

    tr2 = read_text(peer_files("p2")[0])
    check("P2 silent peer: TIME.GET requested exactly twice "
          "(attempt + one retry)",
          count_re(r"TIME\.GET", tr2) == 2)

    tr3 = read_text(peer_files("p3")[0])
    check("P3 clean peer: TIME.GET served (recovery)",
          "TIME.GET" in tr3)

    tr4 = read_text(peer_files("p4")[0])
    st4 = read_json(peer_files("p4")[1])
    check("P4 drop peer: exactly 3 truncated responses",
          count_re(r"\(truncated, drop\)", tr4) == 3)
    check("P4 drop peer: TIME.GET requested exactly 4 times "
          "(probe + attempt + retry + probe)",
          count_re(r"TIME\.GET", tr4) == 4)
    check("P4 drop peer: WIFI.PASS requested exactly once "
          "(never retried)",
          count_re(r"WIFI\.PASS", tr4) == 1)
    check("P4 drop peer: KV.SET ulinkaud_nr requested exactly "
          "once (never retried)",
          count_re(r"KV\.SET ulinkaud_nr", tr4) == 1)
    check("P4 drop peer: exactly 1 KV SET landed",
          st4.get("kv_sets") == 1)
    check("P4 drop peer: KV.GET ulinkaud_missing requested "
          "exactly once (peer ERR, no retry)",
          count_re(r"KV\.GET ulinkaud_missing", tr4) == 1)

    st6 = read_json(peer_files("p6")[1])
    check("P6 peer: 32 AUD ok (restart stream)",
          st6.get("aud_ok") == 32)
    check("P6 peer: 0 bad AUD", st6.get("aud_bad") == 0)
    check("P6 peer: 0 seq gaps", st6.get("aud_seq_gap") == 0)

    tr7 = read_text(peer_files("p7")[0])
    check("P7 junk peer: junk line sent",
          "junk" in tr7)

    tr8 = read_text(peer_files("p8")[0])
    check("P8 peer: over-long KV.SET line served",
          "KV.SET badlen 5000" in tr8)
    check("P8 peer: over-long KV.SET answered ERR ARG",
          "ERR ARG" in tr8)
    check("P8 peer: __hugelen__ requested",
          "__hugelen__" in tr8)
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
print(f"C6L4-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
