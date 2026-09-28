#!/usr/bin/env python3
"""pad-bss.py - Pad binary user dengan nol hingga akhir .bss.

objcopy -O binary tidak menyertakan section NOBITS (.bss) di akhir.
Script ini membaca ELF, mencari .bss, dan mem-pad .bin dengan nol
hingga akhir .bss agar img_len mencakup footprint memori penuh
(loader kernel menghitung npages dari img_len).

Pemakaian: pad-bss.py <prog.bin> <prog.elf>
"""
import subprocess
import sys

binpath, elfpath = sys.argv[1], sys.argv[2]

out = subprocess.run(["llvm-readelf-18", "-S", elfpath],
                     capture_output=True, text=True).stdout
bss_end = 0
for line in out.splitlines():
    if ".bss" in line and "NOBITS" in line:
        p = line.split()
        # Kolom: [ Nr] Name Type Addr Off Size ...
        # p = ['[', '4]', '.bss', 'NOBITS', ADDR, OFF, SIZE, ...]
        bss_end = int(p[4], 16) - 0x100000 + int(p[6], 16)
        break

with open(binpath, "r+b") as f:
    n = len(f.read())
    if bss_end > n:
        f.write(b"\x00" * (bss_end - n))
        print("pad-bss %s: %d -> %d byte" % (binpath, n, bss_end))
