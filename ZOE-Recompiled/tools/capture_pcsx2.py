# DO NOT USE for routine captures: it grabs a region of the desktop, so other windows on top get captured too. Use
# capture_printwindow.py <outdir> <secs> <fps> "<PCSX2 window title part>" (the window's own pixels).
# Passive capture of the PCSX2 game window: grabs the window's area of the desktop at a fixed rate and saves PNGs.
# No input is sent to any window.
# usage: python capture_pcsx2.py <outdir> [seconds=20] [fps=20] [title=Zone of the Enders]
import ctypes, os, queue, sys, threading, time

# Disabled 2026-10-07 (privacy rule: capture only a game window's own pixels, never the desktop). Kept for reference only.
sys.exit('capture_pcsx2.py is disabled: it reads the desktop. Use ZOE-Recompiled/tools/capture_printwindow.py '
         '<outdir> <secs> <fps> "<PCSX2 window title part>" (the window\'s own pixels).')

import win32gui, win32process
from PIL import ImageGrab

ctypes.windll.shcore.SetProcessDpiAwareness(2)  # physical pixels

out = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
fps = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0
title_part = sys.argv[4] if len(sys.argv) > 4 else 'Zone of the Enders'
os.makedirs(out, exist_ok=True)

def find_window():
    best = None
    def visit(hwnd, _):
        nonlocal best
        if not win32gui.IsWindowVisible(hwnd):
            return
        if title_part.lower() not in win32gui.GetWindowText(hwnd).lower():
            return
        l, t, r, b = win32gui.GetClientRect(hwnd)
        area = (r - l) * (b - t)
        if area > 0 and (best is None or area > best[0]):
            best = (area, hwnd)
    win32gui.EnumWindows(visit, None)
    return best[1] if best else None

hwnd = find_window()
if not hwnd:
    sys.exit('no visible window with "%s" in its title' % title_part)
l, t, r, b = win32gui.GetClientRect(hwnd)
x0, y0 = win32gui.ClientToScreen(hwnd, (l, t))
x1, y1 = win32gui.ClientToScreen(hwnd, (r, b))
print('window "%s" client %dx%d at (%d,%d)' % (win32gui.GetWindowText(hwnd), x1 - x0, y1 - y0, x0, y0), flush=True)

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
while True:
    now = time.perf_counter() - start
    if now >= seconds:
        break
    img = ImageGrab.grab(bbox=(x0, y0, x1, y1), all_screens=True)
    frames.put((idx, int(now * 1000), img))
    idx += 1
    nxt = start + idx * period
    delay = nxt - time.perf_counter()
    if delay > 0:
        time.sleep(delay)
frames.put(None)
w.join()
print('captured %d frames in %.1f s -> %s' % (idx, time.perf_counter() - start, out), flush=True)
