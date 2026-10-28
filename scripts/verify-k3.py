#!/usr/bin/env python3
"""K3-VERIFY: virtio-blk probe robustness (K3).

The original theory was a probe race, but boot-count transcripts
proved the real failure class: RUNTIME request loss after a good
probe and mount. A request that times out (lost kick / slow
device) completes late; the old "wait for any used-index
movement" poll then let the NEXT request return on that late
completion -- status 0xff (clobbered by the new request's 0xff
setup) or stale data -- while its own request was still queued.
The fix (K3) waits for used == avail, retries lost kicks across
bounded windows, and retries slot init / signature reads / the
mount itself as defense-in-depth. The SD image itself is healthy,
and the only battery check that catches the failure is SHELL `ls`
(`ls: '/sd' failed`).

A race fix cannot be proven by one boot. This script boots N
times and classifies EVERY boot from its full serial transcript
(chardev logfile: complete from byte 0, no -S pause perturbing
the timing being measured):

  OK             2 devices, `fat32: mounted /sd`, `ls` lists /sd
  PROBE_LOSS     `blk: ready, 1 device(s)` (a slot init failed)
                 and/or `fat32: mount failed`, `ls` fails
  SIG_LOSS       2 devices but the FAT32-signature read lost, so
                 the dev map is swapped (dev 0 = 128 MB SD,
                 dev 1 = 16 MB internal) yet the log claims
                 "dev 0 = internal, dev 1 = SD"; mount fails
  MOUNT_READ_LOSS 2 devices, correct map, but mount still fails
                 (mount's own sd_read(0) lost the race)
  OTHER          boot never reached the shell prompt, or `ls`
                 timed out (not a probe class)

Exit code: 0 iff zero probe/mount/ls failures (OK on every
boot), else 1. Pre-change runs are EXPECTED to fail when the
baseline shows the race; post-change runs must pass.

Harness rules: serial socket drained continuously by a reader
thread; leftover qemu-system-arm killed by exact name first;
per-boot COPIES of both disk images (identical starting state,
no shared-disk mutation); scratch under ~/workspace/logs-k3
(NOT /tmp: /tmp is wiped on VM replacement).
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
KERNEL = os.environ.get(
    "KERNEL", os.path.join(ROOT, "mach3-port", "build", "mach3.elf"))
SHARED_IMG = os.path.join(
    os.path.expanduser("~/workspace/qaonic_os"), "disk-images")
LOGDIR = os.path.expanduser("~/workspace/logs-k3")

TC = os.path.expanduser("~/workspace/toolchain")
ENV = dict(os.environ)
ENV["PATH"] = TC + "/usr/bin:" + ENV.get("PATH", "")
ENV["LD_LIBRARY_PATH"] = (TC + "/usr/lib/x86_64-linux-gnu:"
                          + TC + "/lib/x86_64-linux-gnu:"
                          + ENV.get("LD_LIBRARY_PATH", ""))
ENV["QEMU_MODULE_DIR"] = TC + "/usr/lib/x86_64-linux-gnu/qemu"

SD_SECTORS = 262144   # 128 MB sd128.img
INTERNAL_SECTORS = 32768  # 16 MB mach3-blk.img


def classify(logtext, ls_ok):
    """Return (class, detail) from a full boot transcript."""
    ready = re.search(r"blk: ready, (\d+) device\(s\); SD (\w+)", logtext)
    mounted = "fat32: mounted /sd" in logtext
    mount_failed = "fat32: mount failed" in logtext
    init_failed = len(re.findall(r"blk: slot \d+ init failed", logtext))
    devs = re.findall(r"blk: dev (\d+): slot \d+, (\d+) sectors", logtext)
    swapped_line = "dev 0 = internal (probed 2nd)" in logtext
    if ready is None:
        return ("OTHER", "no blk: ready line (early output lost?)")
    ndev = int(ready.group(1))
    if mounted and ls_ok:
        return ("OK", "mounted, ls ok")
    if ndev < 2:
        return ("PROBE_LOSS",
                "ready=%d devs, init_failed=%d, mount_failed=%s"
                % (ndev, init_failed, mount_failed))
    if mount_failed:
        sizes = {int(d): int(s) for d, s in devs}
        if (sizes.get(0) == SD_SECTORS
                and sizes.get(1) == INTERNAL_SECTORS
                and not swapped_line):
            return ("SIG_LOSS",
                    "dev map swapped (dev0=SD, dev1=internal) "
                    "but log claims internal,SD")
        return ("MOUNT_READ_LOSS",
                "2 devs, map ok, mount failed (mount sd_read lost)")
    if not ls_ok:
        return ("OTHER", "mounted but ls failed (request loss?)")
    return ("OTHER", "unclassified")


def one_boot(idx, tag, timeout_prompt=180, timeout_ls=30):
    """Boot once, return (class, detail, seconds)."""
    work = os.path.join(LOGDIR, "scratch", "%s-boot-%02d" % (tag, idx))
    os.makedirs(work, exist_ok=True)
    blk_img = os.path.join(work, "mach3-blk.img")
    sd_img = os.path.join(work, "sd128.img")
    shutil.copyfile(os.path.join(SHARED_IMG, "mach3-blk.img"), blk_img)
    shutil.copyfile(os.path.join(SHARED_IMG, "sd128.img"), sd_img)
    sock = os.path.join(work, "ser.sock")
    logfile = os.path.join(work, "serial.log")
    serial = bytearray()
    t0 = time.time()

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
            serial.extend(d)

    def conn(path, timeout=60):
        t = time.time()
        while time.time() - t < timeout:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except OSError:
                time.sleep(0.02)
        raise RuntimeError("no socket %s" % path)

    def wait_for(needle, timeout):
        nb = needle.encode()
        t = time.time()
        while time.time() - t < timeout:
            if nb in serial:
                return True
            time.sleep(0.1)
        return False

    qemu = subprocess.Popen(
        ["qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
         "-L", TC + "/usr/share/qemu",
         "-kernel", KERNEL,
         "-chardev",
         "socket,id=ser0,path=%s,server=on,wait=off,logfile=%s"
         % (sock, logfile),
         "-serial", "chardev:ser0",
         "-display", "none",
         "-device", "virtio-gpu-device",
         "-drive", "file=%s,if=none,id=hd0,format=raw" % blk_img,
         "-device", "virtio-blk-device,drive=hd0",
         "-drive", "file=%s,if=none,id=hd1,format=raw" % sd_img,
         "-device", "virtio-blk-device,drive=hd1",
         "-netdev", "user,id=net0,hostfwd=tcp::18084-:80",
         "-device", "virtio-net-device,netdev=net0"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
    try:
        ser = conn(sock)
        ser.settimeout(0.3)
        threading.Thread(target=reader, args=(ser,), daemon=True).start()
        if not wait_for("qaon> ", timeout_prompt):
            return ("OTHER", "no shell prompt", time.time() - t0)
        pos = len(serial)
        ser.sendall(b"ls\n")
        # Wait for either a listing or the failure line.
        ls_ok = None
        t = time.time()
        while time.time() - t < timeout_ls:
            chunk = bytes(serial[pos:]).decode("utf-8", "replace")
            if "/sd:" in chunk and "HELLO.TXT" in chunk:
                ls_ok = True
                break
            if "failed" in chunk:
                ls_ok = False
                break
            time.sleep(0.1)
        if ls_ok is None:
            return ("OTHER", "ls timed out", time.time() - t0)
        time.sleep(0.5)  # let the logfile flush
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
    # The chardev logfile is the complete transcript; prefer it.
    try:
        with open(logfile, "r", encoding="utf-8", errors="replace") as f:
            logtext = f.read()
    except OSError:
        logtext = bytes(serial).decode("utf-8", "replace")
    with open(os.path.join(LOGDIR, "%s-boot-%02d.log" % (tag, idx)),
              "w", encoding="utf-8") as f:
        f.write(logtext)
    cls, detail = classify(logtext, ls_ok)
    shutil.rmtree(work, ignore_errors=True)
    return (cls, detail, time.time() - t0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boots", type=int, default=20)
    ap.add_argument("--tag", default="run")
    args = ap.parse_args()

    subprocess.run(["pkill", "-x", "qemu-system-arm"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    os.makedirs(LOGDIR, exist_ok=True)

    table = []
    for i in range(1, args.boots + 1):
        cls, detail, secs = one_boot(i, args.tag)
        table.append((i, cls, detail, secs))
        print("boot %02d: %-15s %s (%.0fs)"
              % (i, cls, detail, secs), flush=True)
        subprocess.run(["pkill", "-x", "qemu-system-arm"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)

    counts = {}
    for _, cls, _, _ in table:
        counts[cls] = counts.get(cls, 0) + 1
    fails = sum(v for k, v in counts.items() if k != "OK")
    print("K3-BOOTCOUNT tag=%s boots=%d %s fails=%d"
          % (args.tag, args.boots, counts, fails), flush=True)
    if fails:
        print("K3-VERIFY: FAIL (%d/%d boots lost)"
              % (fails, args.boots), flush=True)
        return 1
    print("K3-VERIFY: PASS (%d/%d boots OK)"
          % (args.boots, args.boots), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
