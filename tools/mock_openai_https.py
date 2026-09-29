#!/usr/bin/env python3
"""
tools/mock_openai_https.py -- Mock OpenAI-compatible HTTPS server (Q2c).

Melayani POST /v1/chat/completions dengan skenario scripted 2 langkah:
  POST #1 -> balasan tool_calls: gpio_read pin=40
  POST #2 (body mengandung "role":"tool") -> balasan content final.

TLS dengan cert test yang sama (server.crt/server.key, SAN qabot-test.local,
dari CA test). Berjalan di 127.0.0.1:18444; guest QEMU menjangkau via
10.0.0.2:18444 (slirp hostfwd).

Pemakaian:
  python3 tools/mock_openai_https.py
Berhenti dengan Ctrl-C.
"""
import http.server
import json
import os
import socketserver
import ssl
import sys

PORT = 18444
HERE = os.path.dirname(os.path.abspath(__file__))
CERT = os.path.join(HERE, "..", "user", "tls", "testcerts", "server.crt")
KEY = os.path.join(HERE, "..", "user", "tls", "testcerts", "server.key")

STEP1 = {
    "id": "chatcmpl-mock1",
    "object": "chat.completion",
    "choices": [{
        "index": 0,
        "message": {
            "role": "assistant",
            "content": None,
            "tool_calls": [{
                "id": "call_1",
                "type": "function",
                "function": {
                    "name": "gpio_read",
                    "arguments": "{\"pin\":\"40\"}",
                },
            }],
        },
        "finish_reason": "tool_calls",
    }],
}

STEP2 = {
    "id": "chatcmpl-mock2",
    "object": "chat.completion",
    "choices": [{
        "index": 0,
        "message": {
            "role": "assistant",
            "content": "Pin 40 terbaca.",
        },
        "finish_reason": "stop",
    }],
}


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "MockOpenAI/1.0"
    protocol_version = "HTTP/1.1"  # keep-alive: skenario 2 langkah

    def log_message(self, fmt, *args):  # ringkas
        sys.stderr.write("mock-openai: " + fmt % args + "\n")

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", "replace")
        auth = self.headers.get("Authorization", "")
        self.log_message("POST %s auth=%s body=%dB",
                         self.path,
                         "ok" if auth.startswith("Bearer ") else "MISSING",
                         len(body))
        # Langkah 2 bila request membawa hasil tool.
        payload = STEP2 if '"role":"tool"' in body.replace(" ", "") else STEP1
        data = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        self.wfile.flush()  # penting untuk keep-alive: jangan andalkan close
        # Q2c: tutup koneksi setelah respons (seperti HTTP/1.0). Keep-alive
        # murni (tanpa FIN) memicu isu timing di TCP stack guest (data
        # menetes; TODO investigasi). Untuk uji deterministik, FIN dulu.
        self.close_connection = True


def main():
    for f in (CERT, KEY):
        if not os.path.exists(f):
            sys.stderr.write("mock-openai: cert tak ada: %s\n" % f)
            sys.exit(1)
    httpd = socketserver.TCPServer(("127.0.0.1", PORT), Handler,
                                   bind_and_activate=False)
    httpd.allow_reuse_address = True
    httpd.server_bind()
    httpd.server_activate()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)
    # TLS 1.2 agar selaras dengan client mbedTLS Q2b.
    if hasattr(ssl, "TLSVersion"):
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    sys.stderr.write("mock-openai: HTTPS di 127.0.0.1:%d (TLS1.2)\n" % PORT)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
