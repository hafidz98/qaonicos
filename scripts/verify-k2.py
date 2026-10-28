#!/usr/bin/env python3
"""K2-VERIFY: qabotd registers IPC name `tools` (K2).

C6L-5 verified linkd's TOOL.DISPATCH forwarding ONLY
against the `ulinktools` stub, because qabotd never
registered `tools`. This script proves the real path
end-to-end: peer DISPATCH units (via the mock C6 peer
+ ulink5d TEST.SCRIPT iterations) flow through linkd
to qabotd, qabotd's real policy gate decides, and the
result returns via DISPATCH.RESULT. It also proves
the stub still works (it steals `tools` while alive,
same progid 2) and qabotd recovers the name after
the stub dies (5 s re-register tick).

Shape of qabotd results (vs the stub's canned JSON
objects): qabotd runs the REAL tools and wraps their
k=v output in a JSON string:
  get_info   -> {"ok":true,"result":"uptime_ms=..."}
  gpio_read  -> {"ok":true,"result":"pin=3 val=N"}
  gpio_write -> {"ok":false,"error":"needs_confirm"}
  self_destruct (unknown tool) -> {"ok":false,
    "error":"refused"}
The transcript regexes below key on the STRING form
("result":"uptime_ms), which the stub's object form
("result":{"uptime_ms) does NOT match -- so a
qabotd-shaped result is proof of the real server,
not the stub. CONFIRM non-execution is proved twice:
the needs_confirm verdict AND gpio_read values that
do not change across iterations (the scripted
gpio_write val=1 would flip pin 3 if executed).

Pre-change shape: on the K1 kernel qabotd never
registers `tools`, so every peer dispatch degrades
to tools_unavailable, no QABOTD markers appear, and
no post-stub-death recovery happens; this script
FAILs. Set KERNEL to that ELF to reproduce (PEER
may point at a copy of scripts/mock-c6.py).

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first; the
peer is never left running at the end. Scratch lives
under ~/workspace/logs-k2 (NOT /tmp: /tmp is wiped
on VM replacement). This script needs the QA=1
image (ulinktools is a QA-only program).
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
PEER = os.environ.get("PEER", os.path.join(ROOT, "scripts", "mock-c6.py"))
PORT = 18082
LOGDIR = os.path.expanduser("~/workspace/logs-k2")
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
    return (os.path.join(LOGDIR, f"k2-{tag}-transcript.log"),
            os.path.join(LOGDIR, f"k2-{tag}-stats.json"))


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


SER = os.path.join(LOGDIR, "k2-ser.sock")
MON = os.path.join(LOGDIR, "k2-mon.sock")
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


def shell_cmd(ser, cmd, wait_for, timeout=30):
    # Same lost-input hazard as C6L-5: retry ONLY when
    # the command echo is absent from the segment (a
    # missing echo means the input never reached the
    # shell, so resending cannot double-execute).
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
    # Same read-back-verified put as C6L-5: a garbled
    # put reporting "put: ok" must not pass.
    want = "\n" + val + "\n"
    for _ in range(tries):
        if not shell_cmd(ser, "put /data/link5-test " + val,
                         "put: ok", 20):
            continue
        if shell_cmd(ser, "cat /data/link5-test", want, 20):
            return True
    return False


def run_spawn(ser, cmd, start_marker, timeout=60, tries=3):
    # Same retry rule as C6L-5: retry ONLY on
    # "spawn gagal" or a missing command echo.
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


def read_transcript(tr):
    try:
        with open(tr) as f:
            return f.read()
    except FileNotFoundError:
        return ""


def wait_transcript(tr, pattern, timeout=120):
    rx = re.compile(pattern)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if rx.search(read_transcript(tr)):
            return True
        time.sleep(1.0)
    return rx.search(read_transcript(tr)) is not None


# qabotd-shaped results (string form, NOT the stub's object form).
RE_QABOTD_INFO = r'RESULT \d+ \{"ok":true,"result":"uptime_ms='
RE_QABOTD_GPIO = r'RESULT \d+ \{"ok":true,"result":"pin=3 val=(\d)"\}'

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
    check("qabotd registers name tools",
          wait_from(0, "QABOTD: registered tools", 90))
    check("linkd registers name link",
          wait_from(0, "LINKD: registered link", 90))
    check("ulink5d ready",
          wait_from(0, "ULINK5D: ready", 90))

    # ---- Phase 1: real qabotd serves peer dispatches ----
    # ulink5d iterates TEST.SCRIPT every ~2 s; wait for
    # several qabotd-shaped get_info results, then
    # snapshot the transcript: every phase-1 check
    # reads the snapshot, so the later stub window
    # cannot pollute the attribution.
    tr, _ = peer_paths("a")
    check("qabotd-shaped get_info results arrive",
          wait_transcript(tr, RE_QABOTD_INFO, 150))
    time.sleep(8.0)  # a few more iterations accumulate
    snap = read_transcript(tr)
    gpio_vals = re.findall(RE_QABOTD_GPIO, snap)
    check("phase-1: qabotd-shaped get_info present",
          re.search(RE_QABOTD_INFO, snap) is not None)
    check("phase-1: qabotd gpio_read values present (>=2) "
          "and all equal (write did not execute)",
          len(gpio_vals) >= 2 and len(set(gpio_vals)) == 1)
    check("phase-1: needs_confirm present (gpio_write refused)",
          "needs_confirm" in snap)
    check("phase-1: refused present (self_destruct blocked)",
          '"error":"refused"' in snap)
    check("phase-1: NO tools_unavailable (consumer alive)",
          "tools_unavailable" not in snap)
    check("linkd still UP after refused/unknown dispatches",
          shell_cmd(ser, "link", "link: UP", 30))

    # ---- Phase 2: the stub still works (steals tools) ----
    pos_t = len(serial)
    check("ulinktools started",
          run_spawn(ser, "run embed:ulinktools",
                    "ULINKTOOLS: registered tools", 60))
    check("ulinktools serves 4 dispatches and exits",
          wait_from(pos_t, "ULINKTOOLS: done 4", 150))
    check("shell reports ulinktools exit 0",
          wait_from(pos_t, "exit 0\n", 30))

    # ---- Phase 3: qabotd recovers the name ----
    check("qabotd re-registers tools after stub death",
          wait_from(pos_t, "QABOTD: tools re-registered", 90))
    # A fresh qabotd-shaped result after the recovery
    # marker proves service resumed (not just the
    # registry entry): count occurrences before, then
    # wait for one more.
    n_before = len(re.findall(RE_QABOTD_INFO,
                              read_transcript(tr)))
    ok = False
    t0 = time.time()
    while time.time() - t0 < 120:
        if len(re.findall(RE_QABOTD_INFO,
                          read_transcript(tr))) > n_before:
            ok = True
            break
        time.sleep(1.0)
    check("post-recovery: fresh qabotd-shaped get_info result",
          ok)

    # ---- Teardown: marker cleared, nothing left running ----
    check("marker cleared and reads back 0", set_marker(ser, "0"))
finally:
    try:
        with open(os.path.join(LOGDIR, "k2-serial.log"), "wb") as f:
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
print(f"K2-VERIFY: {'PASS' if npass == len(results) else 'FAIL'} "
      f"({npass}/{len(results)})")
sys.exit(0 if npass == len(results) else 1)
