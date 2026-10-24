import http.client, os, socket, subprocess, time, urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
tc = os.path.expanduser("~/workspace/toolchain")
env = dict(os.environ)
env["PATH"] = tc + "/usr/bin:" + env.get("PATH", "")
env["LD_LIBRARY_PATH"] = (tc + "/usr/lib/x86_64-linux-gnu:" +
    tc + "/lib/x86_64-linux-gnu:" + env.get("LD_LIBRARY_PATH", ""))
env["QEMU_MODULE_DIR"] = tc + "/usr/lib/x86_64-linux-gnu/qemu"
SER = "/tmp/onb1-ser.sock"
for s in (SER,):
    try:
        os.unlink(s)
    except FileNotFoundError:
        pass
p = subprocess.Popen([
    "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "64",
    "-L", tc + "/usr/share/qemu",
    "-kernel", ROOT + "/mach3-port/build/mach3.elf",
    "-drive", f"file={ROOT}/disk-images/mach3-blk.img,if=none,id=hd0,format=raw",
    "-device", "virtio-blk-device,drive=hd0",
    "-drive", f"file={ROOT}/disk-images/sd128.img,if=none,id=hd1,format=raw",
    "-device", "virtio-blk-device,drive=hd1",
    "-netdev", "user,id=net0,hostfwd=tcp::18080-:80",
    "-device", "virtio-net-device,netdev=net0",
    "-display", "none",
    "-serial", f"unix:{SER},server,nowait",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)


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


ser = conn(SER)
os.chmod(SER, 0o777)
ser.settimeout(0.3)
serial = b""

# The guest's console printf is polling-mode: if the serial
# socket is not drained continuously, QEMU backpressure stalls
# the guest CPU inside printf and the netstack dies with it.
# (Found the hard way: HTTP tests hung only when a live serial
# socket was attached but unread.)
import threading


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


def drain_serial(seconds=0.5):
    global serial
    end = time.time() + seconds
    while time.time() < end:
        try:
            d = ser.recv(4096)
            if d:
                serial += d
        except socket.timeout:
            pass


def wait_serial(needle, timeout=60):
    global serial
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle.encode() in serial:
            return True
        drain_serial(0.4)
    return needle.encode() in serial


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


results = []
def check(name, cond):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name)


assert wait_serial("qaon>", 90), "no shell prompt"
time.sleep(2)

st, hd, body = req("GET", "/")
print("step: GET /"); check("GET / dashboard 200", st == 200 and "System Monitor" in body)

st, hd, body = req("GET", "/settings")
check("GET /settings no-cookie -> 303 login",
      st == 303 and "/login" in hd.get("Location", ""))

st, hd, body = req("GET", "/login")
check("GET /login create form", st == 200 and "Buat PIN" in body)

st, hd, body = req("POST", "/login",
                    body=urllib.parse.urlencode({"pin": "246810"}))
cookie = hd.get("Set-Cookie", "").split(";")[0]
print("step: POST login create"); check("POST /login create PIN -> 303 + cookie",
      st == 303 and cookie.startswith("qaon_sess="))

st, hd, body = req("GET", "/settings", cookie=cookie)
check("GET /settings with cookie 200",
      st == 200 and "QaonicOS Settings" in body)

st, hd, body = req("GET", "/shell", cookie=cookie)
check("GET /shell disabled -> mati page",
      st == 200 and "Web shell mati" in body)

st, hd, body = req("POST", "/settings",
                    body=urllib.parse.urlencode(
                        {"name": "qaon-test", "tz": "+0700",
                         "mode": "dev", "shell_on": "on"}),
                    cookie=cookie)
check("POST /settings saved", st == 200 and "Tersimpan" in body)

# cross-check via serial shell: config written by web shows in setup
serial = b""
ser.sendall(b"setup\n")
ok = wait_serial("qaon-test", 15) and wait_serial("pin: set", 5)
check("serial setup shows web-saved name + pin set", ok)

st, hd, body = req("GET", "/shell", cookie=cookie)
check("GET /shell enabled -> form", st == 200 and "Web Shell" in body
      and "Jalankan" in body)

st, hd, body = req("POST", "/shell",
                    body=urllib.parse.urlencode({"cmd": "bootctl"}),
                    cookie=cookie)
check("POST /shell bootctl accepted", st == 200)
got = False
for _ in range(12):
    time.sleep(1)
    st, hd, body = req("GET", "/shell", cookie=cookie)
    if "boot" in body.lower() and "<pre>" in body:
        got = True
        break
check("web shell bootctl output rendered", got)
if got:
    i = body.find("<pre>")
    print("  output sample:", body[i:i + 160].replace("\n", " | "))

# --- B5.5: API key is encrypted at rest (credstore) ---
st, hd, body = req("POST", "/settings",
                    body=urllib.parse.urlencode(
                        {"name": "qaon-test", "tz": "+0700",
                         "mode": "dev", "shell_on": "on",
                         "apikey": "sk-test-1234567890abcdef"}),
                    cookie=cookie)
check("POST /settings with apikey saved",
      st == 200 and "Tersimpan" in body)
serial = b""
ser.sendall(b"cat /data/cfg-apikey\n")
ok = wait_serial("QSEC", 15)
check("cfg-apikey at rest is a QSEC blob", ok)
check("cfg-apikey has no plaintext key",
      b"sk-test-1234567890abcdef" not in serial)

st, hd, body = req("POST", "/login",
                    body=urllib.parse.urlencode({"pin": "999999"}))
check("POST /login wrong PIN rejected",
      st == 200 and "PIN salah" in body)

st, hd, body = req("POST", "/login",
                    body=urllib.parse.urlencode({"pin": "246810"}))
check("POST /login correct PIN -> 303", st == 303)

# serial shell still healthy after web activity
serial = b""
ser.sendall(b"uptime\n")
check("serial shell still responsive", wait_serial("uptime", 10) or
      wait_serial("qaon>", 10))

# --- B5.5: PIN throttle -- 5 consecutive failures lock login ---
for i in range(4):
    st, hd, body = req("POST", "/login",
                       body=urllib.parse.urlencode({"pin": "111111"}))
check("throttle: 4 more wrong PINs still just rejected",
      st == 200 and "PIN salah" in body)
st, hd, body = req("POST", "/login",
                    body=urllib.parse.urlencode({"pin": "111111"}))
check("throttle: 5th failure -> Terkunci (locked)",
      st == 200 and "Terkunci" in body)
st, hd, body = req("POST", "/login",
                    body=urllib.parse.urlencode({"pin": "246810"}))
check("throttle: correct PIN refused while locked",
      st == 200 and "Terkunci" in body)

ser.close()
p.kill()
p.wait()
fails = [n for n, ok in results if not ok]
print("ONB1-VERIFY:", "PASS" if not fails else f"FAIL {fails}")
