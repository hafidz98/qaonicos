#!/usr/bin/env python3
# tools/pem2c.py - Ubah file PEM jadi string C (untuk CA di-embed).
# Pakai: python3 tools/pem2c.py in.pem out.c symbol_name
import sys

inp, outp, sym = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(inp, 'rb').read()

with open(outp, 'w') as f:
    f.write('/* Dibuat oleh tools/pem2c.py dari %s - JANGAN edit manual. */\n' % inp)
    f.write('const char %s[] =\n' % sym)
    # tulis per baris PEM agar mudah dibaca
    text = data.decode('ascii')
    for line in text.splitlines():
        f.write('    "%s\\n"\n' % line)
    f.write('    ;\n')
print('pem2c: %s -> %s (%d byte)' % (inp, outp, len(data)))
