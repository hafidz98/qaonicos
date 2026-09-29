#!/usr/bin/env python3
"""vnc-cap.py -- capture satu frame dari server VNC QEMU (RFB 3.8, Raw).

Pakai: vnc-cap.py [host] [port] [out.png]
Default: 127.0.0.1 5999 /tmp/vnc-cap.png
Murni stdlib (socket + struct + zlib); tidak butuh dependensi.
"""
import socket
import struct
import sys
import zlib


def recvn(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("koneksi VNC terputus")
        buf += chunk
    return buf


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5999
    out = sys.argv[3] if len(sys.argv) > 3 else "/tmp/vnc-cap.png"

    s = socket.create_connection((host, port), timeout=15)

    # Handshake versi.
    ver = recvn(s, 12)
    if not ver.startswith(b"RFB 003."):
        raise RuntimeError(f"versi RFB tak dikenal: {ver!r}")
    s.sendall(ver)  # echo versi yang sama

    # Security: pilih None (1) bila ditawarkan.
    ntypes = recvn(s, 1)[0]
    if ntypes == 0:
        raise RuntimeError("VNC gagal: " + recvn(s, 4).decode(errors="replace"))
    types = recvn(s, ntypes)
    if 1 not in types:
        raise RuntimeError(f"tipe auth tak didukung: {list(types)}")
    s.sendall(b"\x01")
    # Hasil auth (hanya untuk versi < 3.8; 3.8 dengan None tidak ada, tapi
    # QEMU mengirim 4 byte status -- baca toleran).
    s.settimeout(2)
    try:
        res = recvn(s, 4)
        if struct.unpack(">I", res)[0] != 0:
            raise RuntimeError("auth VNC ditolak")
    except socket.timeout:
        pass
    finally:
        s.settimeout(15)

    # ClientInit: shared.
    s.sendall(b"\x01")

    # ServerInit.
    w, h = struct.unpack(">HH", recvn(s, 4))
    pixfmt = recvn(s, 16)
    namelen = struct.unpack(">I", recvn(s, 4))[0]
    name = recvn(s, namelen)
    print(f"server: {w}x{h} name={name!r}", flush=True)

    # SetPixelFormat: 32-bit, depth 24, RGB888 (rshift 16).
    s.sendall(struct.pack(">BxxxBBBBHHHBBBxxx",
                          0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))

    # SetEncodings: Raw saja.
    s.sendall(struct.pack(">BxxxH", 2, 1) + struct.pack(">i", 0))

    # FramebufferUpdateRequest: full, non-incremental.
    s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))

    # Baca update.
    msg = recvn(s, 1)[0]
    if msg != 0:
        raise RuntimeError(f"pesan tak terduga: {msg}")
    recvn(s, 1)  # padding
    nrect = struct.unpack(">H", recvn(s, 2))[0]
    print(f"rects: {nrect}", flush=True)

    fb = bytearray(w * h * 4)
    for _ in range(nrect):
        rx, ry, rw, rh, enc = struct.unpack(">HHHHI", recvn(s, 12))
        if enc != 0:
            raise RuntimeError(f"encoding {enc} tak didukung")
        data = recvn(s, rw * rh * 4)
        for yy in range(rh):
            dst_off = ((ry + yy) * w + rx) * 4
            src_off = yy * rw * 4
            fb[dst_off:dst_off + rw * 4] = data[src_off:src_off + rw * 4]

    s.close()

    # Tulis PNG (RGBA -> RGB, filter 0).
    raw = bytearray()
    for yy in range(h):
        raw.append(0)
        off = yy * w * 4
        for xx in range(w):
            o = off + xx * 4
            # pixel format kita: [31:0] = R:G:B:X (big-endian per spec
            # SetPixelFormat di atas -> byte order network: R,G,B,X)
            raw += bytes((fb[o], fb[o + 1], fb[o + 2]))

    def chunk(typ, data):
        c = typ + data
        return (struct.pack(">I", len(data)) + c +
                struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 6)) +
           chunk(b"IEND", b""))
    with open(out, "wb") as f:
        f.write(png)
    print(f"tersimpan: {out}", flush=True)


if __name__ == "__main__":
    main()
