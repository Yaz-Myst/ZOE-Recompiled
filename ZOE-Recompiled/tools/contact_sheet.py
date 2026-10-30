# Contact sheets of a frame folder: every <step>-th frame, cols x rows thumbnails per sheet, labelled with the file name.
# usage: python contact_sheet.py <framedir> <outdir> [step=3] [cols=6] [rows=6] [thumbw=256] [crop_top=22] [crop_bottom=24]
import os, sys
from PIL import Image, ImageDraw

src, out = sys.argv[1], sys.argv[2]
step = int(sys.argv[3]) if len(sys.argv) > 3 else 3
cols = int(sys.argv[4]) if len(sys.argv) > 4 else 6
rows = int(sys.argv[5]) if len(sys.argv) > 5 else 6
tw = int(sys.argv[6]) if len(sys.argv) > 6 else 256
crop_top = int(sys.argv[7]) if len(sys.argv) > 7 else 22       # PCSX2 menu bar
crop_bottom = int(sys.argv[8]) if len(sys.argv) > 8 else 24    # PCSX2 status bar
os.makedirs(out, exist_ok=True)
files = sorted(f for f in os.listdir(src) if f.endswith('.png'))[::step]
per = cols * rows
for s in range(0, len(files), per):
    batch = files[s:s + per]
    sheet = None
    for i, f in enumerate(batch):
        im = Image.open(os.path.join(src, f)).convert('RGB')
        im = im.crop((0, crop_top, im.width, im.height - crop_bottom))
        th = int(im.height * tw / im.width)
        im = im.resize((tw, th))
        if sheet is None:
            sheet = Image.new('RGB', (cols * tw, rows * (th + 14)), (0, 0, 0))
        x, y = (i % cols) * tw, (i // cols) * (th + 14)
        sheet.paste(im, (x, y + 14))
        ImageDraw.Draw(sheet).text((x + 2, y + 1), f.replace('.png', ''), fill=(255, 255, 0))
    name = os.path.join(out, 'sheet_%03d.png' % (s // per))
    sheet.save(name)
    print(name, batch[0], '..', batch[-1])
