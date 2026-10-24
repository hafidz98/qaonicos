"""ONB-2 verification: unprovisioned boot -> gui -> QR screen
screendump -> ZXing decode == payload from serial log; then
setup done -> finale screen screendump."""
import socket, subprocess, os, sys, time, threading

tc = os.path.expanduser("~/workspace/toolchain")
ROOT = "/home/hatch/workspace/qaonic_os"
SER = "/tmp/onb2-ser.sock"
MON = "/tmp/onb2-mon.sock"
for s in (SER, MON):
    try:
        os.unlink(s)
    except FileNotFoundError:
        pass
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:")
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"
p = subprocess.Popen([
    "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
    "-L", tc + "/usr/share/qemu", "-kernel", ROOT + "/mach3-port/build/mach3.elf",
    "-device", "virtio-gpu-device", "-display", "none",
    "-monitor", f"unix:{MON},server,nowait",
    "-serial", f"unix:{SER},server,nowait",
    "-drive", f"file={ROOT}/disk-images/mach3-blk.img,if=none,id=hd0,format=raw",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive", f"file={ROOT}/disk-images/sd128.img,if=none,id=hd1,format=raw",
    "-device", "virtio-blk-device,drive=hd1",
    "-netdev", "user,id=net0,hostfwd=tcp::18080-:80",
    "-device", "virtio-net-device,netdev=net0",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)


def conn(path):
    for _ in range(60):
        try:
            return socket.socket(socket.AF_UNIX, socket.SOCK_STREAM).connect(
                path) or socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        except OSError:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(path)
                return s
            except OSError:
                time.sleep(0.25)
    raise RuntimeError("no socket " + path)


ser = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(60):
    try:
        ser.connect(SER)
        break
    except OSError:
        time.sleep(0.25)
ser.settimeout(0.3)
log = b""


def _reader():
    global log
    while True:
        try:
            d = ser.recv(4096)
            if d:
                log += d
        except (socket.timeout, OSError):
            pass


threading.Thread(target=_reader, daemon=True).start()


def wait_for(marker, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if marker.encode() in log:
            return True
        time.sleep(0.2)
    return False


def send(cmd):
    ser.sendall(cmd.encode() + b"\n")
    time.sleep(0.6)


def screendump(ppm):
    m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    m.connect(MON)
    m.settimeout(5)
    m.sendall(f"screendump {ppm}\n".encode())
    time.sleep(1.2)
    m.close()


fails = []


def check(name, ok):
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        fails.append(name)


if not wait_for("qaon>"):
    print("NO PROMPT"); sys.exit(1)
send("setup reset")          # unprovisioned
time.sleep(0.5)
send("gui")
check("face reports setup payload",
      wait_for("face: setup mode, payload QAON1:", 20))
payload = None
for line in log.decode(errors="replace").splitlines():
    if "face: setup mode, payload " in line:
        payload = line.split("payload ", 1)[1].strip()
print("payload:", payload)
time.sleep(2)
screendump("/tmp/onb2-qr.ppm")
from PIL import Image
import zxingcpp
img = Image.open("/tmp/onb2-qr.ppm").convert("RGB")
img.save("/tmp/onb2-qr.png")
img_big = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
res = zxingcpp.read_barcode(img_big)
got = res.text if res else None
print("ZXing from framebuffer:", got)
check("framebuffer QR decodes to the serial payload", got == payload)

send("setup done")           # provision -> finale within ~1 s poll
time.sleep(3)
screendump("/tmp/onb2-finale.ppm")
img2 = Image.open("/tmp/onb2-finale.ppm").convert("RGB")
img2.save("/tmp/onb2-finale.png")
# eyes: count eye-coloured pixels in the eye band (blink frames
# shrink the eyes to thin bars, so sample the region, not a point)
cnt = 0
for yy in range(90, 130):
    for xx in range(40, 200):
        r, g, b = img2.getpixel((xx, yy))
        if b > 200 and g > 180 and 100 < r < 200:
            cnt += 1
print("eye pixels:", cnt)
check("finale shows the eyes again", cnt > 200)
check("finale log line", b"face: provisioned -> finale" in log)

p.kill()
print("ONB2-VERIFY:", "PASS" if not fails else "FAIL " + str(fails))
