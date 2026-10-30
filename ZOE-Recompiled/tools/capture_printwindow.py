# Passive capture of one window's own contents (PrintWindow + PW_RENDERFULLCONTENT): windows on top of it, Task View
# or the rest of the desktop are never captured. Grabs as fast as it can (or at most <fps>), PNGs written on a thread.
# usage: python capture_printwindow.py <outdir> [seconds=15] [fps=0 (unlimited)] [title=PS2-Recomp]
import ctypes, os, queue, sys, threading, time

import win32gui, win32ui
from PIL import Image

ctypes.windll.shcore.SetProcessDpiAwareness(2)
out = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
fps = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
title_part = sys.argv[4] if len(sys.argv) > 4 else 'PS2-Recomp'
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
w, h = r - l, b - t
hdc = win32gui.GetWindowDC(hwnd)
src = win32ui.CreateDCFromHandle(hdc)
mem = src.CreateCompatibleDC()
bmp = win32ui.CreateBitmap()
bmp.CreateCompatibleBitmap(src, w, h)
mem.SelectObject(bmp)

frames = queue.Queue()


def writer():
    while True:
        item = frames.get()
        if item is None:
            return
        idx, ms, raw = item
        Image.frombuffer('RGB', (w, h), raw, 'raw', 'BGRX', 0, 1).save(
            os.path.join(out, 'f%05d_%07dms.png' % (idx, ms)), compress_level=1)


th = threading.Thread(target=writer)
th.start()
start = time.perf_counter()
idx = 0
while True:
    now = time.perf_counter() - start
    if now >= seconds:
        break
    ctypes.windll.user32.PrintWindow(hwnd, mem.GetSafeHdc(), 1 | 2)
    frames.put((idx, int(now * 1000), bmp.GetBitmapBits(True)))
    idx += 1
    if fps > 0:
        delay = start + idx / fps - time.perf_counter()
        if delay > 0:
            time.sleep(delay)
elapsed = time.perf_counter() - start
frames.put(None)
th.join()
win32gui.DeleteObject(bmp.GetHandle())
mem.DeleteDC()
src.DeleteDC()
win32gui.ReleaseDC(hwnd, hdc)
print('%d frames in %.1f s (%.1f fps), %dx%d -> %s' % (idx, elapsed, idx / elapsed, w, h, out))
