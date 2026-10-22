#!/usr/bin/env python3
"""Record a full QaonicOS run in QEMU (2026-10-22 build).

Sequence: power on -> kernel splash -> shell -> qfetch card ->
`setup reset` + `gui` -> face QR SETUP screen (ONB-2) ->
`setup done` -> D-GUI-3 finale (eyes + TEKAN TOMBOL UNTUK
MENU + hint) -> serial key 'm' (= EV_MENU, the QEMU stand-in
for the physical button) -> menu grid -> navigate + open the
Monitor screen -> ESC back. Frames via QEMU monitor
screendump; serial is drained continuously by a reader thread
(an undrained serial socket stalls the guest -- see
verify-onb1.py).

Usage: record-full-video.py [out.mp4]
"""
import os, re, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    ROOT, "..", "your_files", "qaonicos-full-qr-setup-v1.mp4")
WORK = os.path.join(ROOT, "logs", "full-video")
FRAMES = os.path.join(WORK, "frames")
os.makedirs(FRAMES, exist_ok=True)
for f in os.listdir(FRAMES):
    os.unlink(os.path.join(FRAMES, f))

tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"

MON = "/tmp/qaon-full-mon.sock"
SER = "/tmp/qaon-full-ser.sock"
for s in (MON, SER):
    try:
        os.unlink(s)
    except FileNotFoundError:
        pass

qemu = subprocess.Popen([
    "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
    "-L", tc + "/usr/share/qemu",
    "-kernel", ROOT + "/kernel/build/mach3.elf",
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


def _reader():
    global transcript
    while True:
        try:
            d = ser.recv(4096)
            if d:
                transcript += d
        except socket.timeout:
            pass
        except OSError:
            return


threading.Thread(target=_reader, daemon=True).start()

fi = 0


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


def hold(seconds, needle=None, timeout=None):
    """Capture frames for `seconds`; if needle is given, first
    wait (up to timeout) for it in the transcript."""
    if needle is not None:
        t0 = time.time()
        while needle not in txt():
            frame()
            time.sleep(0.12)
            if time.time() - t0 > (timeout or 60):
                print(f"TIMEOUT waiting for {needle!r}")
                return False
    t1 = time.time()
    while time.time() - t1 < seconds:
        frame()
        time.sleep(0.12)
    return True


# Phase 1: boot -> splash -> shell prompt (dev mode).
ok = hold(2.5, needle="bootux: dev mode", timeout=150)
print(f"phase1 boot: frames={fi} prompt={'yes' if ok else 'NO'}")

# Phase 2: qfetch (card inserted during assembly, like the
# boot video: the real output rendered as a closing card).
ser.sendall(b"qfetch\n")
qfetch_txt = ""
t1 = time.time()
while time.time() - t1 < 15:
    m = re.search(r"qaon> qfetch\n(.*?)\nqaon> ", txt(), re.S)
    if m:
        qfetch_txt = m.group(1)
        break
    frame()
    time.sleep(0.1)
print("qfetch captured:", "yes" if qfetch_txt else "NO")
open(os.path.join(WORK, "cardpos.txt"), "w").write(str(fi))

# Phase 3: unprovision + gui -> QR setup screen.
ser.sendall(b"setup reset\n")
time.sleep(0.6)
ser.sendall(b"gui\n")
ok = hold(7, needle="face: setup mode, payload", timeout=60)
print(f"phase3 QR screen: frames={fi} ok={ok}")

# Phase 4: provision -> finale (eyes + prompt text + hint).
ser.sendall(b"setup done\n")
ok = hold(8, needle="face: provisioned -> finale", timeout=30)
print(f"phase4 finale: frames={fi} ok={ok}")

# Phase 5: menu via the `key` shell builtin (syscall 105
# event injection -- sh owns the console in QEMU, so raw
# serial keys never reach the GUI; on hardware GPIO feeds
# the same queue). First `key menu` dismisses the finale,
# the second grants the display to uiapp -> menu grid.
ser.sendall(b"key menu\n")
time.sleep(1.5)
ser.sendall(b"key menu\n")
ok = hold(6)
print(f"phase5 menu grid: frames={fi}")
ser.sendall(b"key right\n")
ok = hold(1.5)
ser.sendall(b"key ok\n")
ok = hold(5)
print(f"phase5 screen opened: frames={fi}")
ser.sendall(b"key back\n")
ok = hold(4)
print(f"phase5 back to grid: frames={fi}")

qemu.kill()
qemu.wait()
open(os.path.join(WORK, "qfetch.txt"), "w").write(qfetch_txt)

# Insert the qfetch card right after phase 2's frame count:
# simpler and robust -- append it before the last 40% marker
# is not knowable here; instead the card becomes the frame
# right after the frames captured up to phase 2. We recorded
# that boundary below in the first version of this script by
# tracking fi at phase transitions.
print(f"TOTAL frames={fi}")

# --- assemble: convert, splice the qfetch card at the phase-2
# boundary saved in frames/.cardpos (written by phase 2 code
# path via env var file), encode.
from PIL import Image, ImageDraw, ImageFont
import glob

for ppm in sorted(glob.glob(os.path.join(FRAMES, "*.ppm"))):
    Image.open(ppm).save(ppm.replace(".ppm", ".png"))
    os.unlink(ppm)

pngs = sorted(glob.glob(os.path.join(FRAMES, "*.png")))
if not pngs:
    raise SystemExit("no frames captured")
first = Image.open(pngs[0])
W, H = first.size
card = Image.new("RGB", (W, H), (12, 14, 20))
dr = ImageDraw.Draw(card)
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
def mono(sz):
    if os.path.exists(FONT):
        return ImageFont.truetype(FONT, sz)
    return ImageFont.load_default()
# Layout measured to fit 240x240 with no overflow (owner
# feedback 2026-10-22: the old card truncated the ASCII art
# mid-glyph and its info list spilled off the bottom right).
# Header, then the 5 art lines intact (the art block is 42
# chars; at 9px mono a char is ~5.4px -> 227px wide), then
# the info fields as a clean list.
dr.text((8, 60), "qaon> qfetch", fill=(120, 220, 140), font=mono(12))
raw_lines = qfetch_txt.strip().splitlines()
art = [ln[:43].rstrip() for ln in raw_lines[:5]]
labels = ("OS:", "SoC:", "CPU:", "Uptime:", "Memory:",
          "Threads:", "Net:", "Shell:", "Agent:", "Display:")
info = []
for ln in raw_lines:
    for lb in labels:
        i = ln.find(lb)
        if i >= 0:
            info.append(ln[i:].strip())
            break
y = 82
for ln in art:
    if ln:
        dr.text((8, y), ln, fill=(225, 230, 240), font=mono(9))
    y += 11
y += 8
for ln in info:
    dr.text((8, y), ln, fill=(140, 200, 220), font=mono(10))
    y += 13
cardpos_file = os.path.join(WORK, "cardpos.txt")
cardpos = None
if os.path.exists(cardpos_file):
    cardpos = int(open(cardpos_file).read().strip())
if cardpos is None or cardpos >= len(pngs):
    cardpos = len(pngs) // 3
# shift frames after cardpos up by 10 and drop the card in
for i in range(len(pngs) - 1, cardpos - 1, -1):
    os.rename(pngs[i], os.path.join(FRAMES, f"f{i + 10:04d}.png"))
for k in range(10):
    card.save(os.path.join(FRAMES, f"f{cardpos + k:04d}.png"))

cmd = ["ffmpeg", "-y", "-framerate", "2.5", "-i",
       os.path.join(FRAMES, "f%04d.png"),
       "-c:v", "libx264", "-pix_fmt", "yuv420p", "-r", "30",
       "-movflags", "+faststart", OUT]
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[-1500:])
    raise SystemExit("ffmpeg failed")
print(f"VIDEO OK: {OUT} ({os.path.getsize(OUT)} bytes)")
