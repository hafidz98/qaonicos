#!/usr/bin/env python3
"""Mock HTTP/1.1 keep-alive server for F1 TCP investigation.

Listens on 0.0.0.0:18082. For each connection: reads a request, sends a
~4KB response with Content-Length, then keeps the connection OPEN
(no FIN) for 30s to exercise the guest's keep-alive RX path.
"""
import socket
import threading
import time

PORT = 18082
BODY = b"x" * 4096
RESP = (b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: text/plain\r\n"
        b"Content-Length: " + str(len(BODY)).encode() + b"\r\n"
        b"Connection: keep-alive\r\n"
        b"\r\n" + BODY)


def handle(conn, addr):
    try:
        conn.settimeout(35)
        # Read request (until blank line).
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(1024)
            if not chunk:
                return
            data += chunk
        print(f"[{addr}] request {len(data)}B, sending {len(RESP)}B response",
              flush=True)
        conn.sendall(RESP)
        # Keep open (no FIN) — the F1 scenario.
        time.sleep(30)
    except Exception as e:
        print(f"[{addr}] {e}", flush=True)
    finally:
        conn.close()


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(5)
    print(f"mock keep-alive on :{PORT}", flush=True)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr),
                         daemon=True).start()


if __name__ == "__main__":
    main()
