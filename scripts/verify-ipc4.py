#!/usr/bin/env python3
"""IPC4-VERIFY: kernel registry + RPC + uipc.h v1 freeze (IPC-4).

Boot 1 (dev mode, typed serial -- sh owns the console):
  - lookup-before-register -> ENORV (-9); a refused
    reserved claim has no side effect (lookup still ENORV).
  - reserved names refused with a distinct error:
    `tools` (owner qabotd, progid 2) and `link` (unbound
    until C6L-5) claimed from sh -> EPERM (-13).
  - open names work: register myname, lookup -> send
    name + generation 1, gen -> 1.
  - poll negatives on sh's own port: qlimit 2, two
    sends ok, third -> EFULL (-4), pollrecv drains both,
    third pollrecv -> EEMPTY (-5), oversize -> E2BIG (-6),
    destroy -> send -> EDEST (-3).
  - anti-rendezvous proof: a leg that registers `gone`
    and exits leaves a DEAD entry -- lookup -> EDEST
    (-3, not a live port as the deleted RV gave), and
    the death bumped the generation to 2.
  - table-full: a leg registering 8 names gets 8 ok and
    the 9th -> EBUSY (-7). Runs last: earlier dead
    entries (myname, gone) are evicted by design.

Boot 2 (GUI boot; commands via the web shell after `gui`):
  - A marker-gated client daemon (uipcld, in
    daemon_images, dormant without /data/iptoolsc-test
    == "1") and an embed-hook mock tools server
    (uiptoolssrv, kernel-assigned progid 2) are the two
    live IPC speakers -- the first true concurrent
    send-wake proof in this track:
      server parks in blocking recv; every client RPC
      send wakes it (server counts message-wakes vs
      deadline-wakes; etime must be 0 across the flow,
      and 100 RPCs must finish far faster than any
      deadline-driven schedule).
  - Flow (generations baseline-relative since K2:
    qabotd owns "tools" from GUI start, so the stub's
    incarnation 1 observes gen N=2; the bumps are the
    contract): lookup gen N -> 100 echo RPCs byte-exact
    -> structured dispatch RPC -> double-reply probe
    (server's second reply fails) -> DIE RPC (server
    exits without replying; client gets EPEER -12) ->
    re-lookup -> EDEST + generation N+1 (death bump) ->
    server incarnation 2 registers -> lookup gen N+2 ->
    10 echo RPCs -> STOP -> DONE.
  - Teardown: marker back to "0" via the web shell.

Boot 3 (same disk): shell alive, fail_count=0, marker
reads back "0" (the client daemon is dormant again).

Pre-change shape: on the IPC-3 kernel the new sub-ops
are the deleted RV_* (3/4/5) and SYS_RPC* is ENOSYS, so
the reserved probes get publish-success instead of
EPERM, the generation probe gets a claimed port name
instead of 1, and every RPC fails -- this script FAILs.

Harness rules: the serial socket is drained
continuously by a reader thread; leftover
qemu-system-arm is killed by exact name first;
post-`gui` shell commands go through the web shell
(typed serial races the face's console drain).
"""
import http.client, os, re, socket, subprocess, sys
import threading, time, urllib.parse

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

SER = "/tmp/ipc4-ser.sock"
MON = "/tmp/ipc4-mon.sock"
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


def req(method, path, body=None, cookie=None):
    c = http.client.HTTPConnection("127.0.0.1", 18080, timeout=6)
    headers = {}
    if body is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if cookie:
        headers["Cookie"] = cookie
    c.request(method, path, body=body, headers=headers)
    r = c.getresponse()
    data = r.read().decode("utf-8", "replace")
    hdrs = dict(r.getheaders())
    c.close()
    return r.status, hdrs, data


def launch_qemu():
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


def run_prog(name, timeout=60):
    pos = len(serial)
    ser.sendall(f"run embed:{name}\n".encode())
    wait_from(pos, "exit ", timeout)
    time.sleep(0.3)
    return txt()[pos:]


def ipc_cmd(cmd, wait_for, timeout=30):
    """One `ipc ...` builtin; returns the segment after it."""
    pos = len(serial)
    ser.sendall(cmd.encode() + b"\n")
    wait_from(pos, wait_for, timeout)
    time.sleep(0.2)
    return txt()[pos:]


# ================= Boot 1 =================
check("boot 1 reaches qaon> prompt", wait_from(0, "qaon>", 150))
check("boot 1 shell alive (bootctl)", shell_cmd("bootctl", "fail_count=", 30))

seg = ipc_cmd("ipc lookup tools", "IPC: lookup", 20)
check("lookup-before-register -> ENORV (-9)",
      "IPC: lookup tools rc=-9" in seg)
seg = ipc_cmd("ipc register tools", "IPC: reg", 20)
check("reserved `tools` claimed from sh -> EPERM (-13)",
      "IPC: reg tools rc=-13" in seg)
seg = ipc_cmd("ipc lookup tools", "IPC: lookup", 20)
check("refused claim has no side effect (lookup still ENORV)",
      "IPC: lookup tools rc=-9" in seg)
seg = ipc_cmd("ipc register link", "IPC: reg", 20)
check("unbound reserved `link` -> EPERM (-13)",
      "IPC: reg link rc=-13" in seg)
seg = ipc_cmd("ipc register myname", "IPC: reg", 20)
m = re.search(r"IPC: reg myname rc=0 port=(\d+)", seg)
check("open name registers ok", m is not None)
seg = ipc_cmd("ipc lookup myname", "IPC: lookup", 20)
m2 = re.search(r"IPC: lookup myname send=(\d+) gen=1$", seg, re.M)
check("lookup returns send name + generation 1", m2 is not None)
seg = ipc_cmd("ipc gen myname", "IPC: gen", 20)
check("gen reads back 1", "IPC: gen myname gen=1" in seg)

seg = ipc_cmd("ipc qlimit 2", "IPC: qlimit", 20)
check("qlimit 2 on own port ok", "IPC: qlimit rc=0" in seg)
seg = ipc_cmd("ipc send 11", "IPC: send", 20)
check("self-send 1 ok", "IPC: send rc=0" in seg)
seg = ipc_cmd("ipc send 12", "IPC: send", 20)
check("self-send 2 ok (queue now full)", "IPC: send rc=0" in seg)
seg = ipc_cmd("ipc send 13", "IPC: send", 20)
check("third self-send -> EFULL (-4)", "IPC: send rc=-4" in seg)
seg = ipc_cmd("ipc pollrecv", "IPC: pollrecv", 20)
check("pollrecv drains first (id 11)",
      "IPC: pollrecv rc=" in seg and " id=11" in seg)
seg = ipc_cmd("ipc pollrecv", "IPC: pollrecv", 20)
check("pollrecv drains second (id 12)",
      "IPC: pollrecv rc=" in seg and " id=12" in seg)
seg = ipc_cmd("ipc pollrecv", "IPC: pollrecv", 20)
check("pollrecv on empty -> EEMPTY (-5)", "IPC: pollrecv rc=-5" in seg)
seg = ipc_cmd("ipc sendbig 14", "IPC: sendbig", 20)
check("oversize send -> E2BIG (-6)", "IPC: sendbig rc=-6" in seg)
seg = ipc_cmd("ipc destroy", "IPC: destroy", 20)
check("destroy own port ok", "IPC: destroy rc=0" in seg)
seg = ipc_cmd("ipc send 15", "IPC: send", 20)
check("send to destroyed port -> EDEST (-3)", "IPC: send rc=-3" in seg)

seg = run_prog("uipcregexit")
check("regexit leg registers + exits 0",
      "UIPCREGEXIT: registered gone" in seg and "exit 0\n" in seg)
seg = ipc_cmd("ipc lookup gone", "IPC: lookup", 20)
check("lookup of dead-owner entry -> EDEST (-3), not a live port",
      "IPC: lookup gone rc=-3" in seg)
seg = ipc_cmd("ipc gen gone", "IPC: gen", 20)
check("death bumped the generation to 2",
      "IPC: gen gone gen=2" in seg)

seg = run_prog("uipcregfull")
check("table-full leg exits 0", "exit 0\n" in seg)
check("table-full: 8 registrations ok",
      "UIPCREGFULL: ok 8/8" in seg)
check("table-full: 9th registration -> EBUSY (-7)",
      "UIPCREGFULL: ninth rc=-7" in seg)
check("boot 1: no kernel fault anywhere", "FAULT" not in txt())
check("boot 1: shell responsive at the end (fail_count=0)",
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

st, hd, body = req("POST", "/login",
                   body=urllib.parse.urlencode({"pin": "246810"}))
cookie = hd.get("Set-Cookie", "").split(";")[0]
check("boot 2: web login creates PIN + session",
      st == 303 and cookie.startswith("qaon_sess="))
st, hd, body = req("POST", "/settings",
                   body=urllib.parse.urlencode(
                       {"name": "qaon-ipc4", "tz": "+0700",
                        "mode": "dev", "shell_on": "on"}),
                   cookie=cookie)
check("boot 2: web settings enable the web shell", st == 200)


def web_cmd(cmd):
    st, hd, body = req("POST", "/shell",
                       body=urllib.parse.urlencode({"cmd": cmd}),
                       cookie=cookie)
    return st, body


check("boot 2: marker set to 1",
      shell_cmd("put /data/iptoolsc-test 1", "put: ok", 20))

pos = len(serial)
ser.sendall(b"gui\n")
check("boot 2: GUI daemons launched",
      wait_from(pos, "dashboard daemons launched", 60))
time.sleep(3)

pos = len(serial)
st, body = web_cmd("run embed:uiptoolssrv")
check("boot 2: web shell accepted run embed:uiptoolssrv", st == 200)
check("server#1 registered `tools`",
      wait_from(pos, "UIPTOOLSSRV: registered tools", 60))
t_flow = time.time()
ipc4_base_gen = None
_t0 = time.time()
while time.time() - _t0 < 90:
    _m_gen = re.search(r"UIPCLD: lookup tools gen=(\d+)",
                       txt()[pos:])
    if _m_gen:
        ipc4_base_gen = int(_m_gen.group(1))
        break
    time.sleep(0.5)
check("client lookup tools gen (baseline recorded)",
      ipc4_base_gen is not None)
check("client echo 100/100 byte-exact",
      wait_from(pos, "UIPCLD: PASS echo=100/100", 180))
check("client structured dispatch OK",
      wait_from(pos, "UIPCLD: dispatch OK", 60))
check("server second reply failed (send-once consumed)",
      wait_from(pos, "UIPTOOLSSRV: double2 rc=-", 60))
check("client double-reply first reply received",
      wait_from(pos, "UIPCLD: double first id=", 60))
check("client DIE RPC -> EPEER (-12), not a hang",
      wait_from(pos, "UIPCLD: die rc=-12", 90))
check("client re-lookup after death -> EDEST",
      wait_from(pos, "UIPCLD: regen lookup rc=-3", 60))
check("death bumped generation by 1",
      ipc4_base_gen is not None and
      wait_from(pos, f"UIPCLD: regen gen={ipc4_base_gen + 1}", 60))
pos2 = len(serial)
st, body = web_cmd("run embed:uiptoolssrv")
check("boot 2: web shell accepted server incarnation 2", st == 200)
check("server#2 registered `tools`",
      wait_from(pos2, "UIPTOOLSSRV: registered tools", 60))
check("client lookup2 gen +2 (re-register bump)",
      ipc4_base_gen is not None and
      wait_from(pos2, f"UIPCLD: lookup2 gen={ipc4_base_gen + 2}", 90))
check("client echo2 10/10 on incarnation 2",
      wait_from(pos2, "UIPCLD: PASS echo2=10/10", 120))
check("client DONE, server STOP served",
      wait_from(pos2, "UIPCLD: DONE", 60))
flow_s = time.time() - t_flow
print(f"  concurrent flow wall time: {flow_s:.1f} s", flush=True)
check("concurrent flow far faster than deadline-driven "
      "(100 RPCs would take 100x30 s on deadline wakes)",
      flow_s < 300)
seg2 = txt()[pos:]
m_wake = re.findall(r"UIPTOOLSSRV: wakes msg=(\d+) etime=(\d+)", seg2)
tot_msg = sum(int(a) for a, _ in m_wake)
tot_etime = sum(int(b) for _, b in m_wake)
print(f"  server wake counts: msg={tot_msg} etime={tot_etime}", flush=True)
check("server message-wakes >= 114 (100 echo + dispatch + "
      "double + die + 10 echo2 + stop)",
      tot_msg >= 114)
check("server deadline-wakes == 0 across the RPC flow",
      tot_etime == 0)
check("boot 2: no FAIL marker from either test program",
      "UIPCLD: FAIL" not in seg2 and "UIPTOOLSSRV: FAIL" not in seg2)
check("boot 2: no watchdog verdict during the flow",
      "watchdog:" not in seg2)
check("boot 2: no kernel fault anywhere", "FAULT" not in seg2)

def web_shell_out(cmd, expect, timeout=30):
    """Run cmd through the web shell and wait for expect in the
    command's output. POST /shell only QUEUES the command (the
    endpoint is async: the POST returns a "running" page and the
    output arrives on a later GET refresh; a busy executor answers
    "Shell sibuk" and drops the command). The old single-POST body
    check could therefore never pass, and a busy submit lost the
    teardown put entirely (run 1)."""
    for _ in range(5):
        st, body = web_cmd(cmd)
        if st == 200 and "Shell sibuk" not in body:
            break
        time.sleep(2)
    else:
        return False
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            st, hd, body = req("GET", "/shell", cookie=cookie)
        except Exception:
            time.sleep(2)
            continue
        if st == 200 and expect in body:
            return True
        time.sleep(2)
    return False


check("teardown: marker back to 0",
      web_shell_out("put /data/iptoolsc-test 0", "put: ok"))
p.kill()
time.sleep(1.0)

# ================= Boot 3 =================
for s_ in (SER, MON):
    try:
        os.unlink(s_)
    except FileNotFoundError:
        pass
base3 = len(serial)
p, ser, mon = launch_qemu()
check("boot 3 reaches qaon> prompt", wait_from(base3, "qaon>", 150))
check("boot 3: shell alive + healthy (fail_count=0)",
      shell_cmd("bootctl", "fail_count=0", 30))
seg = ipc_cmd("cat /data/iptoolsc-test", "\n0\n", 20)
check("boot 3: marker reads back 0 (daemon dormant again)",
      "\n0\n" in seg)

p.kill()
with open(os.path.join(ROOT, "logs", "ipc4-serial.log"), "wb") as fh:
    fh.write(serial)
fails = [n for n, ok in results if not ok]
print(f"IPC4-VERIFY: {'PASS' if not fails else 'FAIL'} "
      f"({len(results) - len(fails)}/{len(results)} checks)")
if fails:
    print("failed:", fails)
sys.exit(1 if fails else 0)
