#!/usr/bin/env python3
"""
proxy_horde.py -- Penerjemah OpenAI-format -> AI Horde (Q4c).

Dengarkan 127.0.0.1:18090 (guest QEMU via slirp: 10.0.2.2:18090).
Terima POST /v1/chat/completions (JSON OpenAI), teruskan sebagai
prompt ke AI Horde text API (anonim, gratis, tanpa API key), kembalikan
respons format OpenAI chat.completions.

Jalur: qabotd (guest, TCP polos) -> proxy ini -> egress proxy ->
       stablehorde.net -> model crowdsourced.

AI Horde = antrean bersama; tiap chat butuh submit + poll (10-60 dtk).
Jangan dipakai untuk workload sensitif (prompt terlihat publik).

Jalankan: python3 proxy_horde.py [port] [model]
"""
import json
import os
import socket
import threading
import time
import urllib.request

HORDE = "https://stablehorde.net"
APIKEY = "0000000000"  # kunci anonim AI Horde
DEFAULT_MODEL = "aphrodite/DeepSeek-V4.1-Flash"

SYSTEM = (
    "Kamu Qabot, asisten AI yang jalan di OS kecil (QaonicOS). "
    "Jawab singkat dalam Bahasa Indonesia.\n"
    "Kamu punya tool:\n"
    "- get_info: statistik sistem (uptime, memori)\n"
    "- get_time: jam dinding\n"
    "- gpio_read pin=N: baca pin GPIO\n"
    "- gpio_write pin=N val=0/1: tulis pin GPIO\n"
    "- file_read path=P: baca file (maks 511 byte)\n"
    "- file_write path=P data=D: tulis file\n"
    "- file_list path=P: daftar isi direktori\n"
    "- sys_uptime: ms sejak boot\n"
    "- net_status: info TCP/IP\n"
    "Bila perlu tool, jawab HANYA satu baris format: TOOL:nama k=v k=v\n"
    "contoh: TOOL:gpio_read pin=40\n"
    "contoh: TOOL:file_write path=/halo.txt data=tes\n"
    "Bila tidak perlu tool, jawab langsung teks finalnya."
)


def _opener():
    # Hormati proxy egress dari environment (HTTP_PROXY/HTTPS_PROXY).
    return urllib.request.build_opener(
        urllib.request.ProxyHandler({
            "http": os.environ.get("HTTP_PROXY", ""),
            "https": os.environ.get("HTTPS_PROXY", ""),
        }))


def horde_ask(prompt, model, max_tokens=220):
    op = _opener()
    body = json.dumps({
        "prompt": prompt,
        "params": {"max_new_tokens": max_tokens, "temperature": 0.7},
        "models": [model],
    }).encode()
    gid = None
    for attempt in range(3):
        try:
            req = urllib.request.Request(
                HORDE + "/api/v2/generate/text/async", data=body,
                headers={"apikey": APIKEY,
                         "Content-Type": "application/json"})
            with op.open(req, timeout=60) as r:
                gid = json.load(r)["id"]
            break
        except Exception:
            time.sleep(5)
    if not gid:
        return ""
    for _ in range(18):  # poll maks ~90 dtk (di bawah timeout guest)
        time.sleep(5)
        try:
            req = urllib.request.Request(
                HORDE + "/api/v2/generate/text/status/" + gid,
                headers={"apikey": APIKEY})
            with op.open(req, timeout=30) as r:
                st = json.load(r)
        except Exception:
            continue  # transient (IncompleteRead dsb) -> poll lagi
        if st.get("done"):
            gens = st.get("generations") or []
            if gens:
                return gens[0].get("text", "")
            return ""
    return ""


def build_prompt(messages):
    parts = ["System: " + SYSTEM]
    for m in messages:
        role = m.get("role", "user")
        content = m.get("content", "")
        if role == "system":
            parts.append("System: " + content)
        elif role == "assistant":
            parts.append("Assistant: " + content)
        elif role == "tool":
            parts.append("Tool: " + content)
        else:
            parts.append("User: " + content)
    parts.append("Assistant:")
    return "\n".join(parts)


def handle(conn, model):
    try:
        f = conn.makefile("rb")
        line = f.readline().decode("iso-8859-1")
        if not line:
            return
        parts = line.split()
        if len(parts) < 2 or parts[0] != "POST":
            resp_body = b'{"error":"only POST"}'
        else:
            clen = 0
            while True:
                h = f.readline().decode("iso-8859-1")
                if h in ("\r\n", "\n", ""):
                    break
                if h.lower().startswith("content-length:"):
                    clen = int(h.split(":", 1)[1].strip())
            raw = f.read(clen) if clen else b"{}"
            try:
                req = json.loads(raw.decode("utf-8", "replace"))
                msgs = req.get("messages", [])
                prompt = build_prompt(msgs)
                text = horde_ask(prompt, model).strip()
                if not text:
                    text = "(proxy: AI Horde tak merespons tepat waktu)"
            except Exception as e:  # noqa: BLE001
                text = "(proxy error: %s)" % e
            out = {
                "id": "qabot-horde",
                "object": "chat.completion",
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant",
                               "content": text},
                    "finish_reason": "stop",
                }],
            }
            resp_body = json.dumps(out).encode()
        head = ("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n"
                % len(resp_body)).encode()
        conn.sendall(head + resp_body)
    except Exception:  # noqa: BLE001
        pass
    finally:
        try:
            conn.close()
        except Exception:  # noqa: BLE001
            pass


def main():
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18090
    model = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_MODEL
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
    print("proxy_horde: 127.0.0.1:%d -> AI Horde [%s]" % (port, model),
          flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn, model),
                         daemon=True).start()


if __name__ == "__main__":
    main()
