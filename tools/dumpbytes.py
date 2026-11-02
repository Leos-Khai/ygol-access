"""Prints bytes of YuGiOh.exe at a virtual address (image base 0x140000000), for writing scan patterns.

Usage: python -I tools/dumpbytes.py <YuGiOh.exe> 0xADDR [count]
"""
import struct
import sys

exe = open(sys.argv[1], "rb").read()
addr, count = int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 64
pe = struct.unpack_from("<I", exe, 0x3C)[0]
sections, opt_size = struct.unpack_from("<H", exe, pe + 6)[0], struct.unpack_from("<H", exe, pe + 20)[0]
for i in range(sections):
    name, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", exe, pe + 24 + opt_size + i * 40)
    rva = addr - 0x140000000
    if va <= rva < va + rawsize:
        data = exe[rawptr + rva - va:rawptr + rva - va + count]
        print(" ".join(f"{b:02X}" for b in data))
        break
