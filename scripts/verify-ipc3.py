#!/usr/bin/env python3
"""IPC3-VERIFY: port lifecycle on daemon death (IPC-3).

Script-driven, one dev-mode boot (typed serial only --
no `gui`, so sh owns the console, the FND-4 pattern).
sh is the one persistent userspace task, so the test
holds send names in sh via the test-only `ipc`
builtins (a spawn child cannot hold a name across
another task's death: it cannot spawn, and its space
dies with it -- the first driver design failed on
exactly this, "FAIL spawn rc=-1"):

  P1 clean death:   setup -> resolve -> send ->
                    clean-die (claim + clean exit) ->
                    send must be -3 (UIPC_EDEST).
  P2 watchdog kill: setup -> resolve -> send ->
                    wedge (claim + yield forever;
                    FND-3 warns at 15 s, kills at
                    ~30 s, parent woken with 0xD09D)
                    -> send must be -3.
  P3 fault death:   setup -> resolve -> send ->
                    fault (claim + null deref, exit
                    0xDAB7) -> send must be -3.
  P4 fresh instance: setup -> drain (queue EMPTY:
                    nothing leaked from a dead
                    instance) -> setup -> resolve ->
                    send -> drain (message received:
                    the fresh port genuinely works).
  P5 peer death:    setup -> resolve -> req (reply-
                    carrying request; the wait on the
                    reply port is peer-annotated) ->
                    clean-die (the peer port is
                    destroyed by reclaim) -> park
                    5000 ms returns the kernel's own
                    send-once notice (rc=24 id=71:
                    the queued request died with the
                    peer); a second park 500 ms
                    returns -12 (UIPC_EPEER) at once
                    via the IPC-3 annotation; a third
                    park 500 ms returns -10 (ETIME,
                    control: annotation consumed).

Expected evidence (guest behaviour, not code):
  - every "IPC: ..." line with the expected rc, every
    run exits 0 (53405 for the wedge, 55991 for the
    fault), no FAIL marker from any test program.
  - the kernel death note: bootlog carries event 18
    ("ipc dead ...") from at least one reclaim.
  - shell responsive at the end, fail_count=0.

Pre-change shape: daemon_reclaim has no IPC teardown,
so a dead child's ports leak (still ACTIVE in its
never-freed space): the send-after-death probes
SUCCEED (rc=0, message queued on the corpse port)
instead of -3, and the P5 park expires with ETIME
instead of EPEER -- this script FAILs at P1.

Honest staging limit (see DEVLOG-IPC3): a waiter
parked in a blocking recv while a SECOND, concurrent
actor dies cannot be staged with one spawn slot --
the harness has no concurrent IPC speaker until
IPC-4's registry server. P5 therefore stages the
peer-death verdict at park time (peer already
destroyed by reclaim when the wait opens) plus the
reclaim scan that wakes already-parked waiters; the
mid-park wake differs only in timing, not in code
path (same flag, same re-dispatch verdict).

Harness rules: the serial socket is drained
continuously by a reader thread (guest printf is
polling-mode); leftover qemu-system-arm is killed by
exact name first. IPC state is RAM-only, so nothing
on the dev disk needs scrubbing or restoring.
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

SER = "/tmp/ipc3-ser.sock"
MON = "/tmp/ipc3-mon.sock"
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


def shell_cmd(cmd, wait_for, timeout=30):
    pos = len(serial)
    ser.sendall(cmd.encode() + b"\n")
    return wait_from(pos, wait_for, timeout)


def shell_seg(cmd, wait_for, timeout=30):
    """Type a command, wait for a marker, return the segment."""
    pos = len(serial)
    ser.sendall(cmd.encode() + b"\n")
    wait_from(pos, wait_for, timeout)
    time.sleep(0.3)
    return txt()[pos:]


def run_prog(name, timeout=240):
    """`run embed:<name>`; returns the serial segment."""
    return shell_seg(f"run embed:{name}", "exit ", timeout)


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

check("boot reaches qaon> prompt", wait_from(0, "qaon>", 150))
check("shell alive (bootctl)", shell_cmd("bootctl", "fail_count=", 30))

# --- P1: clean death ---
seg = run_prog("uipcdsetup")
check("P1 setup publishes + exits 0",
      "UIPCDSETUP: port published" in seg and "exit 0\n" in seg)
seg = shell_seg("ipc resolve ipc3", "IPC: hold ", 20)
check("P1 resolve holds a send name", "IPC: hold " in seg and
      "IPC: hold -" not in seg)
seg = shell_seg("ipc send 49153", "IPC: send rc=", 20)
check("P1 send to live port rc=0", "IPC: send rc=0" in seg)
seg = run_prog("uipcdclean")
check("P1 clean child exits 0",
      "UIPCDCLEAN: claimed" in seg and "exit 0\n" in seg)
seg = shell_seg("ipc send 49154", "IPC: send rc=", 20)
check("P1 send-after-death -> EDEST (-3)",
      "IPC: send rc=-3" in seg)

# --- P2: watchdog kill ---
seg = run_prog("uipcdsetup")
check("P2 setup publishes + exits 0",
      "UIPCDSETUP: port published" in seg and "exit 0\n" in seg)
shell_seg("ipc resolve ipc3", "IPC: hold ", 20)
seg = shell_seg("ipc send 49155", "IPC: send rc=", 20)
check("P2 send to live port rc=0", "IPC: send rc=0" in seg)
seg = run_prog("uipcdwedge", timeout=120)
check("P2 wedge killed by watchdog (exit 53405)",
      "exit 53405" in seg)
seg = shell_seg("ipc send 49156", "IPC: send rc=", 20)
check("P2 send-after-watchdog-kill -> EDEST (-3)",
      "IPC: send rc=-3" in seg)

# --- P3: fault death ---
seg = run_prog("uipcdsetup")
check("P3 setup publishes + exits 0",
      "UIPCDSETUP: port published" in seg and "exit 0\n" in seg)
shell_seg("ipc resolve ipc3", "IPC: hold ", 20)
seg = shell_seg("ipc send 49157", "IPC: send rc=", 20)
check("P3 send to live port rc=0", "IPC: send rc=0" in seg)
seg = run_prog("uipcdfault")
check("P3 fault child exits 0xDAB7 (55991)",
      "exit 55991" in seg)
seg = shell_seg("ipc send 49158", "IPC: send rc=", 20)
check("P3 send-after-fault -> EDEST (-3)",
      "IPC: send rc=-3" in seg)

# --- P4: fresh incarnation ---
seg = run_prog("uipcdsetup")
check("P4 setup publishes + exits 0",
      "UIPCDSETUP: port published" in seg and "exit 0\n" in seg)
seg = run_prog("uipcdrain")
check("P4 fresh queue is EMPTY (nothing leaked)",
      "UIPCDRAIN: DRAIN EMPTY" in seg and "exit 0\n" in seg)
seg = run_prog("uipcdsetup")
check("P4 second setup publishes (slot free, not EBUSY)",
      "UIPCDSETUP: port published" in seg and "exit 0\n" in seg)
shell_seg("ipc resolve ipc3", "IPC: hold ", 20)
seg = shell_seg("ipc send 49159", "IPC: send rc=", 20)
check("P4 send to fresh port rc=0", "IPC: send rc=0" in seg)
seg = run_prog("uipcdrain")
check("P4 fresh port carries a real message",
      "UIPCDRAIN: DRAIN MSG OK" in seg and "exit 0\n" in seg)

# --- P5: peer death ---
seg = run_prog("uipcdsetup")
check("P5 setup publishes + exits 0",
      "UIPCDSETUP: port published" in seg and "exit 0\n" in seg)
shell_seg("ipc resolve ipc3", "IPC: hold ", 20)
seg = shell_seg("ipc req 49160", "IPC: req reply=", 20)
check("P5 reply-carrying request rc=0",
      "IPC: req reply=" in seg and " rc=0" in seg)
seg = run_prog("uipcdclean")
check("P5 peer child exits 0",
      "UIPCDCLEAN: claimed" in seg and "exit 0\n" in seg)
# The peer's death produces TWO signals, in order.
# (1) Upstream Mach's own send-once notice: destroying
# the peer port discards the queued request, and
# destroying a message that carries a send-once right
# queues a MACH_NOTIFY_SEND_ONCE (id 71, header-only)
# on the reply port (ipc_kmsg.c -> ipc_notify_send_once).
# The first park polls it out at once -- this is the
# kernel's peer-death channel, and it arrives BEFORE
# any wait record opens, so the IPC-3 annotation is
# still pending afterwards.
seg = shell_seg("ipc park 5000", "IPC: park rc=", 30)
check("P5 first park gets send-once notice (rc=24 id=71)",
      "IPC: park rc=24 id=71" in seg)
# (2) The IPC-3 annotation: the pending peer survived
# the notification delivery (poll returned before
# park-open), so the second park adopts it, finds the
# peer already destroyed, and returns EPEER at once --
# a 500 ms park returning -12 cannot be a deadline
# expiry (that yields -10).
seg = shell_seg("ipc park 500", "IPC: park rc=", 20)
check("P5 second park returns EPEER (-12) at once",
      "IPC: park rc=-12" in seg)
# (3) Control: the annotation is consumed, so a third
# park is a plain deadline park -> ETIME.
seg = shell_seg("ipc park 500", "IPC: park rc=", 20)
check("P5 control park returns ETIME (-10)",
      "IPC: park rc=-10" in seg)

check("no FAIL marker from any test program",
      "UIPCDSETUP: FAIL" not in txt() and
      "UIPCDCLEAN: FAIL" not in txt() and
      "UIPCDFAULT: FAIL" not in txt() and
      "UIPCDWEDGE: FAIL" not in txt() and
      "UIPCDRAIN: FAIL" not in txt())
# Expected FAULT text: P3's deliberate data abort
# (sched: 'spawned' FAULT 0xdab7) and the fault-child
# program's own name (UIPCDFAULT). Anything else is a
# real kernel fault.
_rest = "\n".join(l for l in txt().split("\n")
                  if "FAULT 0xdab7" not in l
                  and "UIPCDFAULT" not in l)
check("no kernel fault during the whole run",
      "FAULT" not in _rest)

# The kernel death note: bootlog event 18. The `bootlog`
# shell command prints "boot #N ev=E ... <note>" lines.
pos = len(serial)
ser.sendall(b"bootlog\n")
wait_from(pos, "records", 20)
time.sleep(0.3)
bl = txt()[pos:]
check("bootlog carries IPC death note (ev 18)",
      "ipc dead" in bl)
check("shell responsive at the end (fail_count=0)",
      shell_cmd("bootctl", "fail_count=0", 30))

p.kill()
with open(os.path.join(ROOT, "logs", "ipc3-serial.log"), "wb") as fh:
    fh.write(serial)
fails = [n for n, ok in results if not ok]
print(f"IPC3-VERIFY: {'PASS' if not fails else 'FAIL'} "
      f"({len(results) - len(fails)}/{len(results)} checks)")
if fails:
    print("failed:", fails)
sys.exit(1 if fails else 0)
