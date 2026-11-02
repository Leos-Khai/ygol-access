"""Counts matches of byte patterns (same syntax as src/scan.cpp) in YuGiOh.exe's .text section.

Usage: python tools/patcheck.py <YuGiOh.exe> "<pattern>"...
"""
import re
import struct
import sys

exe = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", exe, 0x3C)[0]
sections, opt_size = struct.unpack_from("<H", exe, pe + 6)[0], struct.unpack_from("<H", exe, pe + 20)[0]
for i in range(sections):
    name, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", exe, pe + 24 + opt_size + i * 40)
    if name.rstrip(b"\0") == b".text":
        text = exe[rawptr:rawptr + min(vsize, rawsize)]
        break

for pattern in sys.argv[2:]:
    regex = b"".join(b"." if t.startswith("?") else re.escape(bytes([int(t, 16)])) for t in pattern.split())
    hits = [0x140000000 + va + m.start() for m in re.finditer(b"(?=" + regex + b")", text, re.S)]
    print(len(hits), [hex(h) for h in hits[:5]], pattern)
