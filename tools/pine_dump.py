"""Dump PCSX2 EE RAM (32 MiB) via PINE to a file. usage: pine_dump.py <out.bin> [start] [size]"""
import sys, time
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine_read import Pine
out = sys.argv[1]
start = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
size = int(sys.argv[3], 0) if len(sys.argv) > 3 else 32 * 1024 * 1024
p = Pine(); t = time.time()
with open(out, 'wb') as f:
    step = 1 << 16
    for a in range(start, start + size, step):
        f.write(p.read(a, step))
print(f"dumped {size:#x} bytes from {start:#x} in {time.time()-t:.1f}s")
