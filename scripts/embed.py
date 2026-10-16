#!/usr/bin/env python3
# Turn a file into a C header with a byte array: embed.py <input> <output> <name>
import sys

src, dst, name = sys.argv[1:4]
data = open(src, "rb").read()
with open(dst, "w") as f:
    f.write(f"// Generated from {src.split('/')[-1]} by embed.py\n#pragma once\n")
    f.write(f"static const unsigned char {name}[] = {{\n")
    for i in range(0, len(data), 20):
        f.write("  " + ",".join(str(b) for b in data[i:i + 20]) + ",\n")
    f.write("};\n")
