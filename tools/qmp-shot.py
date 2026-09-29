#!/usr/bin/env python3
"""qmp-shot.py -- screenshot display QEMU via QMP screendump.

Pakai: qmp-shot.py [sock] [out.ppm]
Default: /tmp/qmp-a1.sock /tmp/qmp-shot.ppm
"""
import json
import socket
import sys


def main():
    sock_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/qmp-a1.sock"
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/qmp-shot.ppm"

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    f = s.makefile("rwb")

    def read_msg():
        line = f.readline()
        if not line:
            raise RuntimeError("QMP terputus")
        return json.loads(line)

    def cmd(name, args=None):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        f.write((json.dumps(msg) + "\n").encode())
        f.flush()
        while True:
            m = read_msg()
            if "return" in m or "error" in m:
                return m

    greeting = read_msg()  # sapaan QMP
    r = cmd("qmp_capabilities")
    if "error" in r:
        raise RuntimeError(f"qmp_capabilities: {r}")
    r = cmd("screendump", {"filename": out, "format": "ppm"})
    if "error" in r:
        raise RuntimeError(f"screendump: {r}")
    print(f"tersimpan: {out}", flush=True)
    s.close()


if __name__ == "__main__":
    main()
