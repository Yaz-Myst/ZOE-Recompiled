"""Records PCSX2's game window and a block of EE memory at the same instants (passive: no input is sent).

usage: pine_capture_combo.py <outdir> [seconds=30] [hz=8] [global_ptr=0x48ee94] [size=0x2000] [title=Zone of the Enders]
  The block address is read from the global pointer each sample (ZOE: the player object).
Output in <outdir>: frame_NNNN.png (client area), mem.npy [samples, size], times.npy, bases.npy.
Frames are the window's own pixels (PrintWindow + PW_RENDERFULLCONTENT, as capture_printwindow.py): windows on top
of it and the rest of the desktop are never captured.
"""
import ctypes, os, queue, sys, threading, time

import numpy as np
import win32gui, win32ui
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine_read import Pine

ctypes.windll.shcore.SetProcessDpiAwareness(2)

out = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
hz = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0
gptr = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x48EE94
size = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x2000
title_part = sys.argv[6] if len(sys.argv) > 6 else 'Zone of the Enders'
os.makedirs(out, exist_ok=True)


def find_window():
    best = None

    def visit(hwnd, _):
        nonlocal best
        if win32gui.IsWindowVisible(hwnd) and title_part.lower() in win32gui.GetWindowText(hwnd).lower():
            l, t, r, b = win32gui.GetClientRect(hwnd)
            if (r - l) * (b - t) > 0 and (best is None or (r - l) * (b - t) > best[0]):
                best = ((r - l) * (b - t), hwnd)
    win32gui.EnumWindows(visit, None)
    return best[1] if best else None


hwnd = find_window()
if not hwnd:
    sys.exit('no window with "%s" in its title' % title_part)
l, t, r, b = win32gui.GetClientRect(hwnd)
cw, ch = r - l, b - t
hdc = win32gui.GetWindowDC(hwnd)
src = win32ui.CreateDCFromHandle(hdc)
memdc = src.CreateCompatibleDC()
bmp = win32ui.CreateBitmap()
bmp.CreateCompatibleBitmap(src, cw, ch)
memdc.SelectObject(bmp)

pngs = queue.Queue()


def writer():
    while True:
        item = pngs.get()
        if item is None:
            return
        idx, raw = item
        Image.frombuffer('RGB', (cw, ch), raw, 'raw', 'BGRX', 0, 1).save(
            os.path.join(out, 'frame_%04d.png' % idx), compress_level=1)


w = threading.Thread(target=writer)
w.start()

p = Pine()
rows, times, bases = [], [], []
start = time.perf_counter()
i = 0
while time.perf_counter() - start < seconds:
    chunk = p.read(gptr & ~7, 16)
    base = int.from_bytes(chunk[gptr & 7:(gptr & 7) + 4], 'little')
    rows.append(np.frombuffer(p.read(base, size), dtype=np.uint8).copy())
    times.append(time.perf_counter() - start)
    bases.append(base)
    ctypes.windll.user32.PrintWindow(hwnd, memdc.GetSafeHdc(), 1 | 2)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    pngs.put((i, bmp.GetBitmapBits(True)))
    i += 1
    delay = start + i / hz - time.perf_counter()
    if delay > 0:
        time.sleep(delay)
pngs.put(None)
w.join()
win32gui.DeleteObject(bmp.GetHandle())
memdc.DeleteDC()
src.DeleteDC()
win32gui.ReleaseDC(hwnd, hdc)
np.save(os.path.join(out, 'mem.npy'), np.stack(rows))
np.save(os.path.join(out, 'times.npy'), np.array(times))
np.save(os.path.join(out, 'bases.npy'), np.array(bases))
print('%d samples (%d bytes each, bases %s) + frames -> %s' % (len(rows), size, sorted(set(hex(x) for x in bases)), out))
