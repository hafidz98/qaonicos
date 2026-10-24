#!/usr/bin/env python3
"""Record a QaonicOS GUI boot video (Boot UX dev flow).

Sequence: power on -> kernel splash (logo + progress) -> shell
-> qfetch (its real output becomes a card) -> `gui` command ->
face dashboard. Frames are screendumps via the QEMU monitor;
the shell is driven over the serial socket.

Usage: record-boot-video.py [out.mp4]
"""
import os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    ROOT, "..", "your_files", "qaonicos-boot-qfetch.mp4")
WORK = os.path.join(ROOT, "logs", "boot-video")
FRAMES = os.path.join(WORK, "frames")
os.makedirs(FRAMES, exist_ok=True)
for f in os.listdir(FRAMES):
    os.unlink(os.path.join(FRAMES, f))

tcroot = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tcroot + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tcroot + "/usr/lib/x86_64-linux-gnu:" +
    tcroot + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tcroot + "/usr/lib/x86_64-linux-gnu/qemu"

MON = "/tmp/qaon-vid-mon.sock"
SER = "/tmp/qaon-vid-ser.sock"
for s in (MON, SER):
    try:
        os.unlink(s)
    except FileNotFoundError:
        pass

qemu = subprocess.Popen([
    "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
    "-L", tcroot + "/usr/share/qemu",
    "-kernel", ROOT + "/mach3-port/build/mach3.elf",
    "-drive", f"file={ROOT}/disk-images/mach3-blk.img,if=none,id=hd0,format=raw",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive", f"file={ROOT}/disk-images/sd128.img,if=none,id=hd1,format=raw",
    "-device", "virtio-blk-device,drive=hd1",
    "-netdev", "user,id=net0", "-device", "virtio-net-device,netdev=net0",
    "-device", "virtio-gpu-device",
    "-display", "none",
    "-serial", f"unix:{SER},server,nowait",
    "-monitor", f"unix:{MON},server,nowait",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)


def wait_sock(path):
    for _ in range(100):
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except (ConnectionRefusedError, FileNotFoundError):
                time.sleep(0.2)
        else:
            time.sleep(0.2)
    raise RuntimeError(f"socket {path} never appeared")


mon = wait_sock(MON)
mon.settimeout(5.0)
try:
    mon.recv(4096)
except socket.timeout:
    pass
ser = wait_sock(SER)
ser.settimeout(0.25)
os.chmod(SER, 0o777)

transcript = b""
fi = 0


def drain():
    global transcript
    try:
        d = ser.recv(4096)
        if d:
            transcript += d
    except socket.timeout:
        pass


def frame():
    global fi
    ppm = os.path.join(FRAMES, f"f{fi:04d}.ppm")
    mon.sendall(f"screendump {ppm}\n".encode())
    time.sleep(0.22)
    try:
        mon.recv(4096)
    except socket.timeout:
        pass
    fi += 1


def txt():
    return transcript.decode("utf-8", "replace")


# Phase 1: boot -> splash -> shell prompt + dev-mode banner.
t0 = time.time()
prompt_at = None
while time.time() - t0 < 150:
    drain()
    if prompt_at is None and "bootux: dev mode" in txt():
        prompt_at = time.time()
    frame()
    if prompt_at and time.time() - prompt_at > 2.5:
        break
    time.sleep(0.12)
print(f"phase1: {fi} frames, bootux={'yes' if prompt_at else 'NO'}")

# Phase 2: qfetch (prompt anchored at line start -- the Shell
# line itself contains 'qaon> ' mid-line).
ser.sendall(b"qfetch\n")
qfetch_txt = ""
t1 = time.time()
while time.time() - t1 < 15:
    drain()
    m = re.search(r"qaon> qfetch\n(.*?)\nqaon> ", txt(), re.S)
    if m:
        qfetch_txt = m.group(1)
        break
    time.sleep(0.1)
for _ in range(3):
    frame()
print("qfetch captured:", "yes" if qfetch_txt else "NO")

# Phase 3: insert the qfetch card (real output, rendered).
from PIL import Image, ImageDraw, ImageFont
first = Image.open(os.path.join(FRAMES, "f0000.ppm"))
W, H = first.size
card = Image.new("RGB", (W, H), (12, 14, 20))
dr = ImageDraw.Draw(card)
font = None
for cand in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",):
    if os.path.exists(cand):
        font = ImageFont.truetype(cand, max(13, H // 46))
        break
if font is None:
    font = ImageFont.load_default()
dr.text((36, 30), "qaon> qfetch", fill=(120, 220, 140), font=font)
# The qfetch side-by-side layout is wider than the 240px frame:
# render the ASCII art block, then the info fields (located by
# their labels in the real output) as a list below it.
raw_lines = qfetch_txt.strip().splitlines()
art = [ln[:40].rstrip() for ln in raw_lines[:5]]
labels = ("qaon@", "OS:", "SoC:", "CPU:", "Uptime:", "Memory:",
          "Threads:", "Net:", "Shell:", "Agent:", "Display:")
info = []
for ln in raw_lines:
    for lb in labels:
        i = ln.find(lb)
        if i >= 0:
            info.append(ln[i:].strip())
            break
small = None
for cand in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",):
    if os.path.exists(cand):
        small = ImageFont.truetype(cand, 9)
        break
if small is None:
    small = font
y = 30 + (H // 46) * 2
for ln in art:
    if ln:
        dr.text((36, y), ln, fill=(225, 230, 240), font=small)
    y += 12
y += 4
for ln in info:
    dr.text((36, y), ln, fill=(140, 200, 220), font=small)
    y += 12
for _ in range(10):
    card.save(os.path.join(FRAMES, f"f{fi:04d}.png"))
    fi += 1

# Phase 4: `gui` -> face dashboard.
ser.sendall(b"gui\n")
face_at = None
t2 = time.time()
while time.time() - t2 < 60:
    drain()
    if face_at is None and "face: Qabot server starting" in txt():
        face_at = time.time()
    frame()
    if face_at and time.time() - face_at > 6:
        break
    time.sleep(0.12)
print(f"phase4: face={'yes' if face_at else 'NO'}")

qemu.kill()
qemu.wait()
open(os.path.join(WORK, "qfetch.txt"), "w").write(qfetch_txt)
print(qfetch_txt)

# Assemble.
import glob
for ppm in sorted(glob.glob(os.path.join(FRAMES, "*.ppm"))):
    Image.open(ppm).save(ppm.replace(".ppm", ".png"))
    os.unlink(ppm)
cmd = ["ffmpeg", "-y", "-framerate", "2.5", "-i",
       os.path.join(FRAMES, "f%04d.png"),
       "-c:v", "libx264", "-pix_fmt", "yuv420p", "-r", "30",
       "-movflags", "+faststart", OUT]
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[-1500:])
    raise SystemExit("ffmpeg failed")
print(f"VIDEO OK: {OUT} ({os.path.getsize(OUT)} bytes, {fi} frames)")
