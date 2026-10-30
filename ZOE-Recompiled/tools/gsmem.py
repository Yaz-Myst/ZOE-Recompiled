# GS local memory decoder (4 MB dump from PS2X_DIAG_AT / F9): reads textures in the GS swizzled layouts.
# Tables are parsed from the runtime's headers so both stay in step.
import os
import re
import numpy as np

# <repo root>/PS2Recomp/ps2xRuntime/include/runtime/gs, found from this file's own location (ZOE-Recompiled/tools).
HDR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'PS2Recomp', 'ps2xRuntime',
                                    'include', 'runtime', 'gs'))


def _table(file, name, rows, cols):
    s = open(f'{HDR}\\{file}', encoding='utf-8').read()
    m = re.search(name + r'\[\d+\]\[\d+\]\s*=\s*\{(.*?)\};', s, re.S)
    nums = [int(x) for x in re.findall(r'\d+', m.group(1))]
    return np.array(nums, dtype=np.int64).reshape(rows, cols)


BT32 = _table('ps2_gs_psmct32.h', 'blockTable32', 4, 8)
CT32 = _table('ps2_gs_psmct32.h', 'columnTable32', 8, 8)
BT8 = _table('ps2_gs_psmt8.h', 'blockTable8', 4, 8)
CT8 = _table('ps2_gs_psmt8.h', 'columnTable8', 16, 16)
BT4 = _table('ps2_gs_psmt4.h', 'blockTable4', 8, 4)
CT4 = _table('ps2_gs_psmt4.h', 'columnTable4', 16, 32)
BT16 = _table('ps2_gs_psmct16.h', 'blockTable16', 8, 4)
BT16S = _table('ps2_gs_psmct16.h', 'blockTable16S', 8, 4)
CT16 = _table('ps2_gs_psmct16.h', 'columnTable16', 2, 16)


def _grid(w, h):
    y, x = np.mgrid[0:h, 0:w]
    return x.astype(np.int64), y.astype(np.int64)


def addr32(bp, bw, x, y):
    ppr = max(bw, 1)
    page = (bp >> 5) + (y >> 5) * ppr + (x >> 6)
    bid = (bp & 0x1F) + BT32[(y >> 3) & 3, (x >> 3) & 7]
    return (page << 13) + ((bid >> 5) << 13) + (bid & 0x1F) * 256 + CT32[y & 7, x & 7] * 4


def addr16(bp, bw, x, y, bt=BT16):
    ppr = max(bw, 1)
    page = (bp >> 5) + (y >> 6) * ppr + (x >> 6)
    bid = (bp & 0x1F) + bt[(y >> 3) & 7, (x >> 4) & 3]
    return (page << 13) + ((bid >> 5) << 13) + (bid & 0x1F) * 256 + CT16[y & 1, x & 15] * 2 + ((y >> 1) & 3) * 64


def addr8(bp, bw, x, y):
    ppr = max(bw >> 1, 1)
    page = (bp >> 5) + (y >> 6) * ppr + (x >> 7)
    bid = (bp & 0x1F) + BT8[(y >> 4) & 3, (x >> 4) & 7]
    return (page << 13) + ((bid >> 5) << 13) + (bid & 0x1F) * 256 + CT8[y & 15, x & 15]


def addr4(bp, bw, x, y):  # nibble address
    ppr = max(bw >> 1, 1)
    page = (bp >> 5) + (y >> 7) * ppr + (x >> 7)
    bid = (bp & 0x1F) + BT4[(y >> 4) & 7, (x >> 5) & 3]
    return (page << 14) + ((bid >> 5) << 14) + (bid & 0x1F) * 512 + CT4[y & 15, x & 31]


class GsMem:
    def __init__(self, path):
        self.m = np.fromfile(path, dtype=np.uint8)
        assert self.m.size == 4 * 1024 * 1024

    def ct32(self, bp, bw, w, h):
        x, y = _grid(w, h)
        a = addr32(bp, bw, x, y) & (4 * 1024 * 1024 - 1)
        px = np.stack([self.m[a], self.m[a + 1], self.m[a + 2], self.m[a + 3]], axis=-1)
        return px  # RGBA, alpha 0..128 = 0..1

    def ct16(self, bp, bw, w, h, signed=False):
        x, y = _grid(w, h)
        a = addr16(bp, bw, x, y, BT16S if signed else BT16) & (4 * 1024 * 1024 - 2)
        v = self.m[a].astype(np.uint32) | (self.m[a + 1].astype(np.uint32) << 8)
        r = (v & 31) << 3
        g = ((v >> 5) & 31) << 3
        b = ((v >> 10) & 31) << 3
        al = np.where(v & 0x8000, 128, 0)
        return np.stack([r, g, b, al], axis=-1).astype(np.uint8)

    def t8(self, bp, bw, w, h):
        x, y = _grid(w, h)
        return self.m[addr8(bp, bw, x, y) & (4 * 1024 * 1024 - 1)]

    def t4(self, bp, bw, w, h):
        x, y = _grid(w, h)
        a = addr4(bp, bw, x, y) & (8 * 1024 * 1024 - 1)
        b = self.m[a >> 1]
        return np.where(a & 1, b >> 4, b & 15).astype(np.uint8)

    def clut(self, cbp, cpsm, n):
        # CSM1 layout: 256-entry CLUTs are stored as 16x16 with the PS2's swizzle of columns 8..15 rows.
        if cpsm == 0:
            tab = self.ct32(cbp, 1, 16, 16 if n == 256 else 1).reshape(-1, 4)
        else:
            tab = self.ct16(cbp, 1, 16, 16 if n == 256 else 1, signed=(cpsm == 0xA)).reshape(-1, 4)
        if n == 256:
            idx = np.arange(256)
            sw = (idx & 0xE7) | ((idx & 0x08) << 1) | ((idx & 0x10) >> 1)
            tab = tab[sw]
        return tab[:n]

    def texture(self, tbp, tbw, psm, w, h, cbp=None, cpsm=0):
        if psm == 0x00:
            return self.ct32(tbp, tbw, w, h)
        if psm == 0x01:
            px = self.ct32(tbp, tbw, w, h)
            px[..., 3] = 128
            return px
        if psm in (0x02, 0x0A):
            return self.ct16(tbp, tbw, w, h, signed=(psm == 0x0A))
        if psm in (0x13, 0x14):
            idx = self.t8(tbp, tbw, w, h) if psm == 0x13 else self.t4(tbp, tbw, w, h)
            n = 256 if psm == 0x13 else 16
            if cbp is None:
                g = (idx.astype(np.uint32) * (255 // (n - 1))).astype(np.uint8)
                return np.stack([g, g, g, np.full_like(g, 128)], axis=-1)
            return self.clut(cbp, cpsm, n)[idx]
        if psm in (0x1B, 0x24, 0x2C):  # T8H, T4HL, T4HH: indices in the alpha/high bits of a 32-bit page
            px = self.ct32(tbp, tbw, w, h)
            idx = px[..., 3] if psm == 0x1B else ((px[..., 3] & 15) if psm == 0x24 else (px[..., 3] >> 4))
            g = (idx.astype(np.uint32) * (255 // (255 if psm == 0x1B else 15))).astype(np.uint8)
            return np.stack([g, g, g, np.full_like(g, 128)], axis=-1)
        raise ValueError('psm %x' % psm)


def to_image(px, alpha_bg=True):
    from PIL import Image
    rgb = px[..., :3].astype(np.float32)
    a = np.clip(px[..., 3].astype(np.float32) / 128.0, 0, 1)[..., None]
    if alpha_bg:
        h, w = px.shape[:2]
        yy, xx = np.mgrid[0:h, 0:w]
        checker = np.where(((xx >> 3) + (yy >> 3)) & 1, 60, 30).astype(np.float32)[..., None]
        rgb = rgb * a + checker * (1 - a)
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
