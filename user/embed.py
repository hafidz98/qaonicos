#!/usr/bin/env python3
"""Embed blob biner -> array C.

Pemakaian: embed.py <in.bin> <out.c> <symbol> <maxsize>
Gagal (exit != 0) bila blob melebihi maxsize byte.
"""
import sys

inp, outp, sym = sys.argv[1], sys.argv[2], sys.argv[3]
maxsize = int(sys.argv[4])

with open(inp, "rb") as f:
    data = f.read()

if len(data) > maxsize:
    sys.exit("FAIL: %s %d byte > batas %d byte" % (inp, len(data), maxsize))

with open(outp, "w") as f:
    f.write("/* Dibangkitkan oleh user/embed.py - jangan edit manual. */\n")
    f.write("#include <stdint.h>\n")
    f.write("const uint8_t %s[] = {\n" % sym)
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        f.write("    " + ", ".join("0x%02x" % b for b in chunk) + ",\n")
    f.write("};\n")
    f.write("const unsigned %s_len = %du;\n" % (sym, len(data)))

print("embedded %s: %d byte -> %s" % (inp, len(data), sym))
