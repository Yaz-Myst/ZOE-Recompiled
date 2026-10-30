"""Samples a block of PCSX2 EE memory via PINE at a fixed rate and saves it, for finding which fields change with a game state.

usage: pine_watch_obj.py <out.npy> <addr> <size> [seconds=30] [hz=4] [pointer]
  If 'pointer' is given, <addr> is a global holding the block's address (re-read every sample).
Output: numpy array [samples, size] of bytes, plus <out>.times.npy and <out>.bases.npy.
"""
import os, sys, time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine_read import Pine

out = sys.argv[1]
addr = int(sys.argv[2], 0)
size = int(sys.argv[3], 0)
seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 30.0
hz = float(sys.argv[5]) if len(sys.argv) > 5 else 4.0
indirect = len(sys.argv) > 6 and sys.argv[6] == 'pointer'

p = Pine()
rows, times, bases = [], [], []
start = time.perf_counter()
i = 0
while time.perf_counter() - start < seconds:
    if indirect:
        chunk = p.read(addr & ~7, 16)
        base = int.from_bytes(chunk[addr & 7:(addr & 7) + 4], 'little')
    else:
        base = addr
    rows.append(np.frombuffer(p.read(base, size), dtype=np.uint8).copy())
    times.append(time.perf_counter() - start)
    bases.append(base)
    i += 1
    delay = start + i / hz - time.perf_counter()
    if delay > 0:
        time.sleep(delay)
np.save(out, np.stack(rows))
np.save(out + '.times.npy', np.array(times))
np.save(out + '.bases.npy', np.array(bases))
print('%d samples of %d bytes, bases %s' % (len(rows), size, sorted(set(hex(b) for b in bases))))
