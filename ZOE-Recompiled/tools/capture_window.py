# DO NOT USE for routine captures: it grabs the window's AREA OF THE DESKTOP, so anything on top of the window (Task
# View, other apps, private windows) ends up in the PNGs. Use capture_printwindow.py (the window's own pixels).
# Passive capture of a whole window (title bar and frame included) at a fixed rate, saved as lossless PNGs.
# No input is sent to any window. Follows the window if it moves.
# usage: python capture_window.py <outdir> [seconds=30] [fps=16] [title=PS2-Recomp]
import ctypes, ctypes.wintypes, os, queue, sys, threading, time

# Disabled 2026-10-07 (privacy rule: capture only a game window's own pixels, never the desktop). Kept for reference only.
sys.exit('capture_window.py is disabled: it reads the desktop. Use ZOE-Recompiled/tools/capture_printwindow.py '
         '<outdir> <secs> <fps> ["<window title part>"] (the window\'s own pixels).')

import win32gui
from PIL import ImageGrab

ctypes.windll.shcore.SetProcessDpiAwareness(2)  # physical pixels

out = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
fps = float(sys.argv[3]) if len(sys.argv) > 3 else 16.0
title_part = sys.argv[4] if len(sys.argv) > 4 else 'PS2-Recomp'
os.makedirs(out, exist_ok=True)


def find_window():
    best = None

    def visit(hwnd, _):
        nonlocal best
        if not win32gui.IsWindowVisible(hwnd):
            return
        if title_part.lower() not in win32gui.GetWindowText(hwnd).lower():
            return
        l, t, r, b = win32gui.GetWindowRect(hwnd)
        area = (r - l) * (b - t)
        if area > 0 and (best is None or area > best[0]):
            best = (area, hwnd)
    win32gui.EnumWindows(visit, None)
    return best[1] if best else None


def visible_bounds(hwnd):
    # DWMWA_EXTENDED_FRAME_BOUNDS: the window as drawn (GetWindowRect includes invisible resize borders).
    rect = ctypes.wintypes.RECT()
    if ctypes.windll.dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(rect), ctypes.sizeof(rect)) == 0:
        return rect.left, rect.top, rect.right, rect.bottom
    return win32gui.GetWindowRect(hwnd)


hwnd = find_window()
if not hwnd:
    sys.exit('no visible window with "%s" in its title' % title_part)
box = visible_bounds(hwnd)
print('window "%s" %dx%d at (%d,%d)' % (win32gui.GetWindowText(hwnd), box[2] - box[0], box[3] - box[1], box[0], box[1]),
      flush=True)

frames = queue.Queue()


def writer():
    while True:
        item = frames.get()
        if item is None:
            return
        idx, ms, img = item
        img.save(os.path.join(out, 'f%05d_%07dms.png' % (idx, ms)), compress_level=1)


w = threading.Thread(target=writer)
w.start()

start = time.perf_counter()
period = 1.0 / fps
idx = 0
last_bounds_check = 0.0
while True:
    now = time.perf_counter() - start
    if now >= seconds:
        break
    if now - last_bounds_check > 1.0:
        box = visible_bounds(hwnd)
        last_bounds_check = now
    frames.put((idx, int(now * 1000), ImageGrab.grab(bbox=box, all_screens=True)))
    idx += 1
    delay = start + idx * period - time.perf_counter()
    if delay > 0:
        time.sleep(delay)
frames.put(None)
w.join()
print('captured %d frames in %.1f s -> %s' % (idx, time.perf_counter() - start, out), flush=True)
