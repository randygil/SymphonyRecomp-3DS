#!/usr/bin/env python3
"""bin2c.py out.c name1=file1 [name2=file2 ...] -> const u8 arrays + sizes"""
import sys

out = sys.argv[1]
with open(out, "w") as f:
    f.write('#include "recomp.h"\n')
    for spec in sys.argv[2:]:
        name, path = spec.split("=", 1)
        data = open(path, "rb").read()
        f.write(f"const u8 {name}[{len(data)}] __attribute__((aligned(4))) = {{\n")
        for i in range(0, len(data), 32):
            f.write(",".join(str(b) for b in data[i:i + 32]) + ",\n")
        f.write("};\n")
        f.write(f"const u32 {name}_size = {len(data)};\n")
