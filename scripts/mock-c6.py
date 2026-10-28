#!/usr/bin/env python3
"""mock-c6.py -- host-side mock ESP32-C6 peer for the C6L track.

A wire-level peer for the frozen wire v1.1 (see
docs/rencana-komunikasi-c6/WIRE-FREEZE.md), the C6L-2
addition NET.HTTP, and the C6L-3 wire v1.2 additions
(TOOL.DISPATCH, peer-initiated DISPATCH, EVT frames,
NET.WS framing -- see docs/rencana-komunikasi-c6/
WIRE-FREEZE-V1.2.md). It speaks the exact bytes the
in-process mock (user/uartproto/mock_comcu.c) speaks,
over a TCP connection, so a guest client cannot tell
which peer answered -- except for the deliberate
differences below.

Deliberate differences from the in-process mock:
- State lives in this process's memory. A TCP reconnect
  keeps state (same process); a peer RESTART resets it
  (fresh NVS), like a C6 reboot. The in-process mock
  persists NVS to the guest SD card across guest reboots.
- TIME.GET returns real host wall time (WIB, +07:00).
  The in-process mock returns a MOCK- label before NTP
  sync; this peer is always "synced" (host clock).
- NET.HTTP GET is implemented here (the in-process mock
  lacks it): `NET.HTTP GET <url>` -> `OK <len>\n<body>`,
  a deterministic 1024-byte pattern body. Any other
  NET.HTTP shape is ERR ARG.
- Fault-injection flags: --delay-ms (sleep before every
  response), --silent (accept but never answer),
  --drop-after N (truncate the Nth response mid-way and
  close the connection), --kill-after N (close after N
  completed responses, no reply to the next request),
  --junk-after N (C6L-4: after the Nth completed
  ordinary response, inject one garbage line into the
  stream and keep the connection open -- the next
  guest command reads junk as its response and must
  link-reset, never desync).
  Default: none (a clean peer).
- C6L-4 test keys: KV.GET __badlen__ answers a
  non-numeric length ("OK abc"); KV.GET __hugelen__
  answers "OK 100000" and sends no body (the guest
  must time out, never hang). AUD payloads starting
  with "ULS4" + u16 BE sequence are sequence-checked
  (aud_seq_gap counts gaps; expected resets on
  VOICE.START); other payloads are counted only.

Usage:
  python3 scripts/mock-c6.py [--port 18082]
      [--transcript FILE] [--stats-file FILE]
      [--delay-ms N] [--silent]
      [--drop-after N] [--kill-after N]
      [--scan-list "SSID,RSSI,SEC;..."]

The guest connects to 10.0.2.2:<port> via slirp (no
hostfwd needed for the outbound direction). One client
at a time; on disconnect the peer returns to accept().
Transcript lines (appended, flushed per line):
  SESSION <n> ACCEPT / CLOSE <reason>
  REQ <repr of raw request bytes>   (line + body, or frame)
  RESP <repr of response bytes>
  SEND <repr of scripted outbound bytes> (TEST.SCRIPT /
    TEST.BADFRAME: DISPATCH lines, EVT frames)
  RESULT <id> <result JSON>  (DISPATCH.RESULT receipts)
  FRAME AUD <len> ok|bad-crc|bad-type|outside-session
  FRAME BIN <len> echo|drop|bad-crc
Stats JSON (rewritten after every completed request):
  sessions, commands, aud_ok, aud_bad, aud_outside,
  aud_seq_gap, nethttp_bytes, kv_sets, tool_dispatch,
  dispatch_results, ws_echo, ws_drop.
"""
import argparse
import json
import os
import socket
import sys
import time

# --- Mirrored mock limits (user/uartproto/mock_comcu.c) ---
KV_MAX_SLOTS = 8
KV_KEY_MAX = 31          # key[32] incl. NUL
KV_VAL_STORE_MAX = 63    # val[64] incl. NUL: a 64-byte SET
                         # is accepted but stored as 63
KV_VAL_CLIENT_MAX = 64   # UPROTO_KV_VAL_MAX: over this -> ARG
WIFI_SAVED_MAX = 2
BLE_NAME_MAX = 23        # ble_name[24] incl. NUL
BODY_MAX = 4096          # numeric KV length over this -> ARG
NETHTTP_LEN = 1024

SCAN_DEFAULT = [
    ("RUMAH-HAFIDZ", "-52", "WPA2"),
    ("KANTOR-5G", "-67", "WPA2"),
    ("WARUNGKOPI", "-78", "OPEN"),
]


def crc16_ccitt(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 \
                else (crc << 1) & 0xFFFF
    return crc


def nethttp_body():
    return bytes(((i * 7 + 13) & 0xFF) for i in range(NETHTTP_LEN))


def wib_now():
    t = time.time() + 7 * 3600
    tm = time.gmtime(t)
    return "%04d-%02d-%02dT%02d:%02d:%02d+07:00" % (
        tm.tm_year, tm.tm_mon, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec)


class Peer:
    def __init__(self, args, transcript, stats_path):
        self.args = args
        self.transcript = transcript
        self.stats_path = stats_path
        self.stats = {"sessions": 0, "commands": 0, "aud_ok": 0,
                      "aud_bad": 0, "aud_outside": 0,
                      "aud_seq_gap": 0,
                      "nethttp_bytes": 0, "kv_sets": 0,
                      "tool_dispatch": 0, "dispatch_results": 0,
                      "ws_echo": 0, "ws_drop": 0}
        # C6L-3 state.
        self.ws_open = False
        self.ws_sid = 1
        self.script_results = []  # (id, result JSON) in order
        # NVS state (mirrors mock_comcu.c defaults).
        self.kv = {}              # key -> bytes (<=63 stored)
        self.wifi_saved = []      # list of ssid, max 2
        self.ble_on = False
        self.ble_name = "QABOT-01"
        # Wi-Fi runtime state (not persisted).
        self.wifi_conn = None     # ssid or None
        # K5: initial radio state flags (verify-k5's
        # pre-change run boots the peer already-ON so
        # the old kernel's always-OFF icons fail
        # against live state without any driver).
        if getattr(args, "wifi_on", None):
            self.wifi_conn = args.wifi_on
        if getattr(args, "ble_on", False):
            self.ble_on = True
        self.wifi_pending = None  # ssid awaiting PASS
        # Voice state.
        self.voice_active = False
        self.voice_frames = 0
        self.voice_seq_expected = 0
        self.junk_sent = False
        # Scan list.
        self.scan = list(args.scan)
        self.served = 0           # completed responses, this session

    def log(self, line):
        if self.transcript:
            self.transcript.write(line + "\n")
            self.transcript.flush()

    def write_stats(self):
        if self.stats_path:
            tmp = self.stats_path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(self.stats, f)
            os.replace(tmp, self.stats_path)

    # ---------- command dispatch (mirrors mock_comcu.c) ----------
    def dispatch(self, line, body):
        """line: str without newline. body: bytes or None (KV.SET
        body; None means no body arrived with the line).
        Returns response bytes (without trailing newline added
        by caller -- responses here include it)."""
        self.stats["commands"] += 1
        parts = line.split(" ")
        cmd = parts[0]

        def ok(payload=None):
            return ("OK" + (" " + payload if payload else "") + "\n"
                    ).encode()

        def err(code):
            return ("ERR " + code + "\n").encode()

        # --- Wi-Fi ---
        if line == "WIFI.SCAN":
            out = ["OK %d" % len(self.scan)]
            out += ["%s,%s,%s" % s for s in self.scan]
            return ("\n".join(out) + "\n").encode()
        if cmd == "WIFI.CONNECT" and len(parts) >= 2:
            ssid = parts[1]
            for (s, _r, sec) in self.scan:
                if s == ssid:
                    if sec == "OPEN":
                        self.wifi_conn = ssid
                        self.wifi_pending = None
                        return ok("CONNECTED")
                    self.wifi_pending = ssid
                    return ok("NEEDPASS")
            return err("NOTFOUND")
        if cmd == "WIFI.PASS" and len(parts) >= 2:
            if not self.wifi_pending:
                return err("NOCONN")
            pwd = parts[1]
            if len(pwd) >= 8:
                self.wifi_conn = self.wifi_pending
                self.wifi_pending = None
                if (self.wifi_conn not in self.wifi_saved
                        and len(self.wifi_saved) < WIFI_SAVED_MAX):
                    self.wifi_saved.append(self.wifi_conn)
                return ok("CONNECTED")
            return err("AUTH")
        if line == "WIFI.LIST":
            out = ["OK %d" % len(self.wifi_saved)]
            out += ["%s,WPA2" % s for s in self.wifi_saved]
            return ("\n".join(out) + "\n").encode()
        if cmd == "WIFI.FORGET" and len(parts) >= 2:
            ssid = parts[1]
            if ssid in self.wifi_saved:
                self.wifi_saved.remove(ssid)
            return ok()
        if line == "WIFI.STATUS":
            if self.wifi_conn:
                return ok("CONNECTED " + self.wifi_conn)
            return ok("DISCONNECTED")
        if line == "WIFI.DISC":
            self.wifi_conn = None
            self.wifi_pending = None
            return ok()

        # --- BLE ---
        if line == "BLE.ON":
            self.ble_on = True
            return ok()
        if line == "BLE.OFF":
            self.ble_on = False
            return ok()
        if cmd == "BLE.NAME" and len(parts) >= 2:
            self.ble_name = parts[1][:BLE_NAME_MAX]
            return ok()
        if line == "BLE.STATUS":
            return ok(("ON " if self.ble_on else "OFF ") + "NONE")

        # --- KV (length-prefixed) ---
        if cmd == "KV.SET":
            if body is None:
                return err("ARG")
            if len(parts) < 3:
                return err("ARG")
            key = parts[1]
            if not key or len(body) > KV_VAL_CLIENT_MAX:
                return err("ARG")
            if len(self.kv) >= KV_MAX_SLOTS and key not in self.kv:
                return err("FULL")
            self.kv[key] = body[:KV_VAL_STORE_MAX]
            self.stats["kv_sets"] += 1
            return ok()
        if cmd == "KV.GET" and len(parts) >= 2:
            key = parts[1]
            # C6L-4 test keys (never stored, never
            # looked up): malformed / over-long length.
            if key == "__badlen__":
                return b"OK abc\n"
            if key == "__hugelen__":
                return b"OK 100000\n"
            if key not in self.kv:
                return err("NOTFOUND")
            val = self.kv[key]
            return ("OK %d\n" % len(val)).encode() + val
        if cmd == "KV.DEL" and len(parts) >= 2:
            self.kv.pop(parts[1], None)
            return ok()

        # --- Time / FIDO ---
        if line == "TIME.GET":
            return ok(wib_now())
        if line == "FIDO.LIST":
            return ok("0")

        # --- Voice ---
        if line == "VOICE.START 16000 16":
            self.voice_frames = 0
            self.voice_seq_expected = 0
            self.voice_active = True
            return ok()
        if line == "VOICE.STOP":
            self.voice_active = False
            return ok(str(self.voice_frames))

        # --- NET.HTTP (C6L-2 addition) ---
        if cmd == "NET.HTTP":
            if len(parts) == 3 and parts[1] == "GET":
                body_b = nethttp_body()
                self.stats["nethttp_bytes"] += len(body_b)
                return ("OK %d\n" % len(body_b)).encode() + body_b
            return err("ARG")

        # --- TOOL.DISPATCH (C6L-3, guest -> peer) ---
        # line: "TOOL.DISPATCH <tool> <len>", body: args JSON.
        if cmd == "TOOL.DISPATCH":
            if body is None or len(parts) < 3:
                return err("ARG")
            tool = parts[1]
            self.stats["tool_dispatch"] += 1
            if tool == "get_info":
                res = (b'{"ok":true,"result":{"device":"mock-c6",'
                       b'"link":"v1.2"}}')
                return ("OK %d\n" % len(res)).encode() + res
            if tool == "gpio_read":
                import re as _re
                m = _re.search(rb'"pin"\s*:\s*(\d+)', body)
                if not m:
                    return err("ARG")
                pin = int(m.group(1))
                res = ('{"ok":true,"result":{"pin":%d,'
                       '"value":%d}}' % (pin, pin & 1)).encode()
                return ("OK %d\n" % len(res)).encode() + res
            if tool == "fail_test":
                res = b'{"ok":false,"error":"test_failure"}'
                return ("OK %d\n" % len(res)).encode() + res
            return err("NOTOOL")

        # --- DISPATCH.RESULT (C6L-3, guest answers a
        # peer-initiated dispatch) ---
        # line: "DISPATCH.RESULT <id> <len>", body: result JSON.
        if cmd == "DISPATCH.RESULT":
            if body is None or len(parts) < 3:
                return err("ARG")
            try:
                rid = int(parts[1])
            except ValueError:
                return err("ARG")
            self.script_results.append(
                (rid, body.decode("ascii", "replace")))
            self.stats["dispatch_results"] += 1
            self.log("RESULT %d %s" % (rid, body.decode(
                "ascii", "replace")))
            return ok()

        # --- NET.WS session (C6L-3, framing only) ---
        if cmd == "NET.WS.OPEN":
            if self.ws_open:
                return err("BUSY")
            self.ws_open = True
            return ok("1")
        if cmd == "NET.WS.CLOSE":
            if not self.ws_open or len(parts) < 2 \
                    or parts[1] != "1":
                return err("NOCONN")
            self.ws_open = False
            return ok()

        return err("UNKNOWN")

    # ---------- C6L-3 scripted outbound units ----------
    def script_bytes(self):
        """TEST.SCRIPT c6l3 units, in order: DISPATCH
        get_info, DISPATCH gpio_read, DISPATCH
        gpio_write, DISPATCH self_destruct, then an
        EVT WIFI.DROPPED frame."""
        out = b""
        out += b"DISPATCH 1 get_info 2\n{}"
        out += b'DISPATCH 2 gpio_read 9\n{"pin":3}'
        out += b'DISPATCH 3 gpio_write 17\n{"pin":3,"val":1}'
        out += b"DISPATCH 4 self_destruct 2\n{}"
        payload = b"WIFI.DROPPED"
        hdr = b"QA" + bytes([0x03, (len(payload) >> 8) & 0xFF,
                             len(payload) & 0xFF])
        crc = crc16_ccitt(hdr + payload)
        out += hdr + payload + bytes([(crc >> 8) & 0xFF,
                                      crc & 0xFF])
        return out

    def badframe_bytes(self):
        """One EVT frame with a corrupt CRC (TEST.BADFRAME)."""
        payload = b"WIFI.DROPPED"
        hdr = b"QA" + bytes([0x03, (len(payload) >> 8) & 0xFF,
                             len(payload) & 0xFF])
        crc = crc16_ccitt(hdr + payload) ^ 0xFFFF
        return hdr + payload + bytes([(crc >> 8) & 0xFF,
                                      crc & 0xFF])

    # ---------- frame handling ----------
    def handle_frame(self, frame):
        """frame: full QA frame bytes. Returns echo bytes
        to send back (BIN WS echo while a session is
        open), or None. AUD frames are counted, never
        answered; EVT frames never come from the guest."""
        if len(frame) < 7:
            self.stats["aud_bad"] += 1
            self.log("FRAME short bad")
            return None
        typ = frame[2]
        ln = (frame[3] << 8) | frame[4]
        if len(frame) != 5 + ln + 2:
            self.stats["aud_bad"] += 1
            self.log("FRAME AUD %d bad-len" % ln)
            return None
        crc = (frame[5 + ln] << 8) | frame[5 + ln + 1]
        if crc != crc16_ccitt(frame[:5 + ln]):
            if typ == 0x02:
                self.stats["ws_drop"] += 1
                self.log("FRAME BIN %d bad-crc" % ln)
            else:
                self.stats["aud_bad"] += 1
                self.log("FRAME AUD %d bad-crc" % ln)
            return None
        if typ == 0x02:
            if not self.ws_open:
                self.stats["ws_drop"] += 1
                self.log("FRAME BIN %d drop-no-session" % ln)
                return None
            self.stats["ws_echo"] += 1
            self.log("FRAME BIN %d echo" % ln)
            return frame
        if typ == 0x03:
            self.log("FRAME EVT %d unexpected-from-guest" % ln)
            return None
        if typ != 0x01:
            self.stats["aud_bad"] += 1
            self.log("FRAME type %02x bad-type" % typ)
            return None
        if not self.voice_active:
            self.stats["aud_outside"] += 1
            self.log("FRAME AUD %d outside-session" % ln)
            return None
        self.voice_frames += 1
        self.stats["aud_ok"] += 1
        payload = frame[5:5 + ln]
        if payload[:4] == b"ULS4" and ln >= 6:
            seq = (payload[4] << 8) | payload[5]
            if seq != self.voice_seq_expected:
                self.stats["aud_seq_gap"] += 1
                self.log("FRAME AUD %d seq gap got %d want %d"
                         % (ln, seq, self.voice_seq_expected))
            self.voice_seq_expected = seq + 1
        self.log("FRAME AUD %d ok" % ln)
        return None

    # ---------- connection serving ----------
    def serve(self, conn, session_no):
        buf = bytearray()
        # (kind, arg, want_len) while a body arrives;
        # kind: KV.SET, TOOL.DISPATCH, DISPATCH.RESULT.
        pending_body = None

        def recv_more():
            try:
                data = conn.recv(4096)
            except OSError:
                return False
            if not data:
                return False
            buf.extend(data)
            return True

        while True:
            # -- body phase (KV.SET / TOOL.DISPATCH /
            #    DISPATCH.RESULT) --
            if pending_body is not None:
                kind, arg, want = pending_body
                if len(buf) < want:
                    if not recv_more():
                        return "eof"
                    continue
                body = bytes(buf[:want])
                del buf[:want]
                pending_body = None
                line_out = "%s %s %d" % (kind, arg, want)
                self.log("REQ %r" % (
                    (line_out + "\n").encode() + body))
                resp = self.dispatch(line_out, body)
                if self.args.silent:
                    continue
                if self.args.delay_ms:
                    time.sleep(self.args.delay_ms / 1000.0)
                if (self.args.drop_after is not None
                        and self.served >= self.args.drop_after):
                    half = resp[:len(resp) // 2]
                    conn.sendall(half)
                    self.log("RESP %r (truncated, drop)" % half)
                    return "dropped"
                conn.sendall(resp)
                self.log("RESP %r" % resp)
                self.served += 1
                self.write_stats()
                if (self.args.kill_after is not None
                        and self.served >= self.args.kill_after):
                    return "killed"
                continue

            # -- fresh message: frame or line? --
            if len(buf) < 2:
                if not recv_more():
                    return "eof"
                continue
            if buf[0:1] == b"A" and buf[1:2] == b"U" \
                    and len(buf) >= 3 and buf[2:3] == b"D":
                # Legacy 3-byte-magic frame ("AUD" + len BE +
                # payload + CRC): consume and reject, so the
                # stream never desynchronises on it.
                if len(buf) < 5:
                    if not recv_more():
                        return "eof"
                    continue
                ln = (buf[3] << 8) | buf[4]
                need = 5 + ln + 2
                if len(buf) < need:
                    if not recv_more():
                        return "eof"
                    continue
                del buf[:need]
                self.stats["aud_bad"] += 1
                self.log("FRAME legacy rejected")
                self.write_stats()
                continue
            if buf[0:1] == b"Q" and buf[1:2] == b"A":
                # Frame: need 5 header bytes, then len + CRC.
                if len(buf) < 5:
                    if not recv_more():
                        return "eof"
                    continue
                ln = (buf[3] << 8) | buf[4]
                need = 5 + ln + 2
                if len(buf) < need:
                    if not recv_more():
                        return "eof"
                    continue
                frame = bytes(buf[:need])
                del buf[:need]
                echo = self.handle_frame(frame)
                if echo is not None:
                    conn.sendall(echo)
                self.write_stats()
                continue
            # Text line phase.
            nl = buf.find(b"\n")
            if nl < 0:
                if not recv_more():
                    return "eof"
                continue
            raw = bytes(buf[:nl])
            del buf[:nl + 1]
            line = raw.decode("ascii", "replace")
            if line.endswith("\r"):
                line = line[:-1]
            # KV.SET line: parse key/len, then body phase.
            if line.startswith("KV.SET"):
                toks = line.split(" ")
                if len(toks) != 3 or not toks[2].isdigit():
                    # Legacy inline shape or garbage: ARG at
                    # once, no body wait (mirrors the mock).
                    self.log("REQ %r" % (line + "\n").encode())
                    resp = b"ERR ARG\n"
                    if not self.args.silent:
                        if self.args.delay_ms:
                            time.sleep(self.args.delay_ms / 1000.0)
                        conn.sendall(resp)
                    self.log("RESP %r" % resp)
                    self.stats["commands"] += 1
                    self.served += 1
                    self.write_stats()
                    continue
                want = int(toks[2])
                if want > BODY_MAX:
                    self.log("REQ %r" % (line + "\n").encode())
                    resp = b"ERR ARG\n"
                    if not self.args.silent:
                        conn.sendall(resp)
                    self.log("RESP %r" % resp)
                    self.stats["commands"] += 1
                    self.served += 1
                    self.write_stats()
                    continue
                if want == 0:
                    self.log("REQ %r" % (line + "\n").encode())
                    resp = self.dispatch(line, b"")
                    if not self.args.silent:
                        conn.sendall(resp)
                    self.log("RESP %r" % resp)
                    self.served += 1
                    self.write_stats()
                    continue
                pending_body = ("KV.SET", toks[1], want)
                continue
            # TOOL.DISPATCH / DISPATCH.RESULT lines: same
            # body-phase shape as KV.SET.
            if line.startswith("TOOL.DISPATCH") or \
                    line.startswith("DISPATCH.RESULT"):
                toks = line.split(" ")
                kind = toks[0]
                if len(toks) != 3 or not toks[2].isdigit():
                    self.log("REQ %r" % (line + "\n").encode())
                    resp = b"ERR ARG\n"
                    if not self.args.silent:
                        conn.sendall(resp)
                    self.log("RESP %r" % resp)
                    self.stats["commands"] += 1
                    self.served += 1
                    self.write_stats()
                    continue
                want = int(toks[2])
                if want > BODY_MAX:
                    self.log("REQ %r" % (line + "\n").encode())
                    resp = b"ERR ARG\n"
                    if not self.args.silent:
                        conn.sendall(resp)
                    self.log("RESP %r" % resp)
                    self.stats["commands"] += 1
                    self.served += 1
                    self.write_stats()
                    continue
                if want == 0:
                    self.log("REQ %r" % (line + "\n").encode())
                    resp = self.dispatch(line, b"")
                    if not self.args.silent:
                        conn.sendall(resp)
                    self.log("RESP %r" % resp)
                    self.served += 1
                    self.write_stats()
                    continue
                pending_body = (kind, toks[1], want)
                continue
            # Ordinary text command.
            self.log("REQ %r" % (line + "\n").encode())
            # Test-only scripted outbound units (C6L-3):
            # the response goes out first, then the
            # script bytes. Bypasses fault injection.
            if line == "TEST.SCRIPT c6l3":
                resp = b"OK ARMED\n"
                conn.sendall(resp)
                self.log("RESP %r" % resp)
                script = self.script_bytes()
                conn.sendall(script)
                self.log("SEND %r" % script)
                self.stats["commands"] += 1
                self.served += 1
                self.write_stats()
                continue
            if line == "TEST.BADFRAME":
                resp = b"OK\n"
                conn.sendall(resp)
                self.log("RESP %r" % resp)
                bad = self.badframe_bytes()
                conn.sendall(bad)
                self.log("SEND %r" % bad)
                self.stats["commands"] += 1
                self.served += 1
                self.write_stats()
                continue
            resp = self.dispatch(line, None)
            if self.args.silent:
                continue
            if self.args.delay_ms:
                time.sleep(self.args.delay_ms / 1000.0)
            if (self.args.drop_after is not None
                    and self.served >= self.args.drop_after):
                half = resp[:len(resp) // 2]
                conn.sendall(half)
                self.log("RESP %r (truncated, drop)" % half)
                return "dropped"
            conn.sendall(resp)
            self.log("RESP %r" % resp)
            self.served += 1
            self.write_stats()
            if (self.args.junk_after is not None
                    and self.served >= self.args.junk_after
                    and not self.junk_sent):
                self.junk_sent = True
                junk = b"GARBAGE\xff\n"
                conn.sendall(junk)
                self.log("RESP %r (junk)" % junk)
            if (self.args.kill_after is not None
                    and self.served >= self.args.kill_after):
                return "killed"


def parse_scan(spec):
    out = []
    for item in spec.split(";"):
        item = item.strip()
        if not item:
            continue
        parts = item.split(",")
        if len(parts) == 3:
            out.append((parts[0], parts[1], parts[2]))
    return out or SCAN_DEFAULT


# The accept loop below shares one Peer across sessions,
# so NVS/Wi-Fi/BLE state survives TCP reconnects within
# one process lifetime (a process restart resets it).
def run():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18082)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--transcript", default=None)
    ap.add_argument("--stats-file", default=None)
    ap.add_argument("--delay-ms", type=int, default=0)
    ap.add_argument("--silent", action="store_true")
    ap.add_argument("--drop-after", type=int, default=None)
    ap.add_argument("--kill-after", type=int, default=None)
    ap.add_argument("--junk-after", type=int, default=None)
    ap.add_argument("--scan-list", default=None)
    ap.add_argument("--wifi-on", default=None,
                    help="K5: start already connected to this SSID")
    ap.add_argument("--ble-on", action="store_true",
                    help="K5: start with BLE on")
    args = ap.parse_args()
    args.scan = parse_scan(args.scan_list) if args.scan_list \
        else SCAN_DEFAULT

    transcript = None
    if args.transcript:
        transcript = open(args.transcript, "a", buffering=1)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print("mock-c6: listening on %s:%d" % (args.host, args.port),
          flush=True)
    peer = Peer(args, transcript, args.stats_file)
    peer.write_stats()  # initial zeros, so verify scripts
    # can read stats (and FAIL cleanly) even when no
    # guest session ever connects (pre-change runs).
    session_no = 0
    try:
        while True:
            conn, _addr = srv.accept()
            session_no += 1
            peer.stats["sessions"] = session_no
            peer.served = 0
            peer.junk_sent = False
            peer.write_stats()
            if transcript:
                transcript.write("SESSION %d ACCEPT\n" % session_no)
                transcript.flush()
            reason = peer.serve(conn, session_no)
            try:
                conn.close()
            except OSError:
                pass
            if transcript:
                transcript.write("SESSION %d CLOSE %s\n"
                                 % (session_no, reason))
                transcript.flush()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
        if transcript:
            transcript.close()


if __name__ == "__main__":
    run()
