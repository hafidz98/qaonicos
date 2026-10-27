#!/usr/bin/env python3
"""K1-VERIFY: FPU/NEON save/restore per thread (THR-1).

Value-comparing proof, not boot-PASS:
  1. Kernel boot selftest (tests/fpu_test.c): two kernel threads
     own distinct VFP/NEON patterns (A words 0xA..., B words
     0xB..., distinct FPSCR) and switch cooperatively for 20
     rounds each while the 100 Hz timer is live; every switch
     is followed by a bit-exact pattern check.  The switch
     path proven is __Switch_context (pcb-keyed), which is
     also the path cooperative, stack_handoff, and
     thread_block/thread_dispatch (IPC park/wake) switches
     all funnel through.
  2. Userspace leg (user/ufpu.c): a user-mode VFP pattern
     must survive an IPC park/wake cycle (blocking RECV
     with a 2 s deadline on a private port, waking on
     UIPC_ETIME) and read back bit-exact.
  3. No kernel panic anywhere in the boot log.

Pre-change (selftest + ufpu present, switch fix absent) this
script FAILs: the kernel markers read fpu_selftest FAIL with
MISMATCH lines (B's pattern clobbers A's across switches).
Post-change it PASSes.

Boots QEMU on COPIES of the disk images (logs-k1/), so the
shared dev disk is never touched.  No display/grants needed:
everything is serial markers + one shell command.
"""
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = os.path.expanduser("~/workspace/logs-k1")
MAIN = os.path.expanduser("~/workspace/qaonic_os")
KERNEL = os.path.join(ROOT, "mach3-port/build/mach3.elf")
SER = os.path.join(LOGDIR, "k1-serial.sock")
TC = os.path.expanduser("~/workspace/toolchain")

os.makedirs(LOGDIR, exist_ok=True)
for p in (os.path.join(MAIN, "disk-images/mach3-blk.img"),
          os.path.join(MAIN, "disk-images/sd128.img")):
    shutil.copyfile(p, os.path.join(LOGDIR, os.path.basename(p)))
BLK = os.path.join(LOGDIR, "mach3-blk.img")
SD = os.path.join(LOGDIR, "sd128.img")

serial = bytearray()
lock = threading.Lock()


def reader(s):
    while True:
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        except Exception:
            return
        if not d:
            return
        with lock:
            serial.extend(d)


def conn(path, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"no socket {path}")


def wait_for(needle, timeout):
    nb = needle.encode()
    t0 = time.time()
    while time.time() - t0 < timeout:
        with lock:
            if nb in serial:
                return True
        time.sleep(0.2)
    return False


def kill_qemu():
    try:
        out = subprocess.run(["pgrep", "-x", "qemu-system-arm"],
                             capture_output=True, text=True).stdout
        for pid in out.split():
            try:
                os.kill(int(pid), signal.SIGTERM)
            except OSError:
                pass
        time.sleep(0.5)
    except Exception:
        pass


def main():
    kill_qemu()
    for p in (SER,):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    env = dict(os.environ)
    env["PATH"] = TC + "/usr/bin:" + env["PATH"]
    env["LD_LIBRARY_PATH"] = (TC + "/usr/lib/x86_64-linux-gnu:" +
                              TC + "/lib/x86_64-linux-gnu")
    env["QEMU_MODULE_DIR"] = TC + "/usr/lib/x86_64-linux-gnu/qemu"
    qemu = subprocess.Popen(
        ["qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
         "-L", TC + "/usr/share/qemu",
         "-kernel", KERNEL,
         "-drive", f"file={BLK},if=none,id=hd0,format=raw",
         "-device", "virtio-blk-device,drive=hd0",
         "-drive", f"file={SD},if=none,id=hd1,format=raw",
         "-device", "virtio-blk-device,drive=hd1",
         "-netdev", "user,id=net0",
         "-device", "virtio-net-device,netdev=net0",
         "-serial", f"unix:{SER},server,nowait",
         "-display", "none"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    checks = []
    try:
        ser = conn(SER)
        ser.settimeout(0.3)
        threading.Thread(target=reader, args=(ser,), daemon=True).start()
        ok_prompt = wait_for("qaon> ", 180)
        checks.append(("shell prompt reached", ok_prompt))
        with lock:
            log = bytes(serial).decode("utf-8", "replace")
        m_pass = re.search(r"fpu_selftest: PASS rounds=(\d+) checks=(\d+)",
                           log)
        checks.append(("kernel fpu_selftest PASS marker",
                       m_pass is not None))
        checks.append(("no fpu_selftest FAIL marker",
                       "fpu_selftest: FAIL" not in log))
        checks.append(("no VFP MISMATCH lines",
                       "MISMATCH" not in log))
        checks.append(("no kernel panic",
                       "panic(" not in log and "panic:" not in log))
        if ok_prompt:
            ser.sendall(b"run embed:ufpu\n")
            ok_ufpu = wait_for("UFPU: PASS", 90)
            checks.append(("userspace UFPU PASS (IPC park/wake)",
                           ok_ufpu))
            with lock:
                log2 = bytes(serial).decode("utf-8", "replace")
            checks.append(("no UFPU FAIL marker",
                           "UFPU: FAIL" not in log2))
            wait_for("qaon> ", 30)
        else:
            checks.append(("userspace UFPU PASS (IPC park/wake)", False))
            checks.append(("no UFPU FAIL marker", False))
        with open(os.path.join(LOGDIR, "k1-serial.log"), "wb") as f:
            with lock:
                f.write(bytes(serial))
        fails = [name for name, ok in checks if not ok]
        for name, ok in checks:
            print(f"{'PASS' if ok else 'FAIL'}: {name}")
        if m_pass:
            print(f"kernel rounds={m_pass.group(1)} "
                  f"checks={m_pass.group(2)}")
        print(f"K1-VERIFY: {'PASS' if not fails else 'FAIL'} "
              f"({len(checks) - len(fails)}/{len(checks)})")
        return 0 if not fails else 1
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=10)
        except Exception:
            try:
                qemu.kill()
            except Exception:
                pass
        kill_qemu()


if __name__ == "__main__":
    sys.exit(main())
