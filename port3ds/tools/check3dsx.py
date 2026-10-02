#!/usr/bin/env python3
"""check3dsx.py file.3dsx - flags relocations Azahar/hbl would reject (value top nibble != 0)."""
import struct, sys

d = open(sys.argv[1], 'rb').read()
_, hs, rhs, _, _, code, ro, data, bss = struct.unpack_from('<4sHHIIIIII', d, 0)
rel = [struct.unpack_from('<II', d, hs + s * rhs) for s in range(3)]
off = hs + 3 * rhs
segdata = [d[off:off + code], d[off + code:off + code + ro], d[off + code + ro:off + code + ro + data - bss] + b'\0' * bss]
pos = off + code + ro + data - bss
bad = 0
for s in range(3):
    for t in range(2):
        p = 0
        for _ in range(rel[s][t]):
            skip, patch = struct.unpack_from('<HH', d, pos)
            pos += 4
            p += skip * 4
            for _ in range(patch):
                v = struct.unpack_from('<I', segdata[s], p)[0]
                if v >> 28:
                    bad += 1
                    if bad <= 10:
                        print(f'segment {s} table {t} offset {p:#x} value {v:#x}')
                p += 4
print('bad relocations:', bad)
sys.exit(1 if bad else 0)
