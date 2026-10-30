# Builds a GIF from captured PNG frames (capture_printwindow.py), tuned for clarity: each frame gets its own 256-colour
# palette (median cut over that frame) with Floyd-Steinberg dithering, and frames are written as differences where
# possible.
# usage: python make_gif.py <framedir> <out.gif> [start_ms=0] [end_ms=all] [width=0 (keep)] [fps=16]
import os, re, sys

from PIL import Image

src, out = sys.argv[1], sys.argv[2]
start_ms = int(sys.argv[3]) if len(sys.argv) > 3 else 0
end_ms = int(sys.argv[4]) if len(sys.argv) > 4 and int(sys.argv[4]) > 0 else 1 << 62
width = int(sys.argv[5]) if len(sys.argv) > 5 else 0
fps = float(sys.argv[6]) if len(sys.argv) > 6 else 16.0

files = []
for f in sorted(os.listdir(src)):
    m = re.match(r'f\d+_(\d+)ms\.png$', f)
    if m and start_ms <= int(m.group(1)) <= end_ms:
        files.append(f)
if not files:
    sys.exit('no frames in range')

frames = []
for f in files:
    im = Image.open(os.path.join(src, f)).convert('RGB')
    if width and im.width != width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    frames.append(im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG))

delay = round(1000.0 / fps)
frames[0].save(out, save_all=True, append_images=frames[1:], duration=delay, loop=0, optimize=False, disposal=1)
print('%s: %d frames, %dx%d, %.1f s at %.0f fps, %.1f MB' % (out, len(frames), frames[0].width, frames[0].height,
                                                           len(frames) * delay / 1000.0, fps, os.path.getsize(out) / 1e6))
