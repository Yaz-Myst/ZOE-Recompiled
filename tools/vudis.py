"""Minimal PS2 VU micro-instruction disassembler (upper + lower pipelines).

usage: vudis.py <file> <file_offset> <size> [labels "addr=name,..."]
Addresses printed are VU instruction byte addresses (8 bytes per pair).
"""
import struct
import sys

FIELD = lambda d: "".join(c for c, b in zip("xyzw", (8, 4, 2, 1)) if d & b)
BC = "xyzw"


def upper(w):
    dest = (w >> 21) & 0xF
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    flags = "".join(n for n, b in (("I", 31), ("E", 30), ("M", 29), ("D", 28), ("T", 27)) if w >> b & 1)
    op = w & 0x3F
    d = FIELD(dest)
    bc = BC[w & 3]
    special1 = {0x28: "add", 0x2C: "sub", 0x2A: "mul", 0x29: "madd", 0x2D: "msub", 0x2B: "max", 0x2F: "mini",
                0x2E: "opmsub", 0x1C: "mulq", 0x20: "addq", 0x24: "subq", 0x21: "maddq", 0x25: "msubq",
                0x1E: "muli", 0x22: "addi", 0x26: "subi", 0x23: "maddi", 0x27: "msubi", 0x1D: "maxi", 0x1F: "minii"}
    if op < 0x3C:
        if op < 0x1C:
            grp = op >> 2
            name = ["add", "sub", "madd", "msub", "max", "mini", "mul"][grp] if grp < 7 else "?"
            return f"{name}{bc}.{d} vf{fd}, vf{fs}, vf{ft}{bc}", flags
        if op in special1:
            n = special1[op]
            if n.endswith(("q", "i")) and n not in ("mul", "add", "sub", "madd", "msub"):
                return f"{n}.{d} vf{fd}, vf{fs}", flags
            return f"{n}.{d} vf{fd}, vf{fs}, vf{ft}", flags
        return f"upper?{op:#x}", flags
    # special2: opcode in bits 0-1 | bits 6-10
    op2 = (w & 3) | ((w >> 4) & 0x7C)
    t2 = {0x3C: "addax", 0x3D: "adday", 0x3E: "addaz", 0x3F: "addaw", 0x7C: "subax", 0x7D: "subay", 0x7E: "subaz", 0x7F: "subaw",
          0xBC: "maddax", 0xBD: "madday", 0xBE: "maddaz", 0xBF: "maddaw", 0xFC: "msubax", 0xFD: "msubay", 0xFE: "msubaz", 0xFF: "msubaw",
          0x1BC: "mulax", 0x1BD: "mulay", 0x1BE: "mulaz", 0x1BF: "mulaw", 0x2FF: "nop", 0x1FF: "clip", 0x2FE: "opmula",
          0x13C: "itof0", 0x13D: "itof4", 0x13E: "itof12", 0x13F: "itof15", 0x17C: "ftoi0", 0x17D: "ftoi4", 0x17E: "ftoi12", 0x17F: "ftoi15",
          0x1FC: "abs", 0x2BC: "adda", 0x2FC: "suba", 0x2BD: "madda", 0x2FD: "msuba", 0x2BE: "mula", 0x1FD: "mulaq", 0x23C: "addaq",
          0x27C: "subaq", 0x23D: "maddaq", 0x27D: "msubaq", 0x1FE: "mulai", 0x23E: "addai", 0x27E: "subai", 0x23F: "maddai", 0x27F: "msubai"}
    op2 = ((w >> 6) & 0x1F) << 2 | (w & 3)
    key = op2 | 0x3C if False else None
    full = ((w >> 6) & 0x1F) * 4 + (w & 3) + 0x3C * 0
    code = (((w >> 6) & 0x1F) << 2) | (w & 3)
    # table keyed by the canonical 11-bit value (bits 0-10)
    code11 = w & 0x7FF
    t = {0x03C: "addax", 0x03D: "adday", 0x03E: "addaz", 0x03F: "addaw", 0x07C: "subax", 0x07D: "subay", 0x07E: "subaz", 0x07F: "subaw",
         0x0BC: "maddax", 0x0BD: "madday", 0x0BE: "maddaz", 0x0BF: "maddaw", 0x0FC: "msubax", 0x0FD: "msubay", 0x0FE: "msubaz", 0x0FF: "msubaw",
         0x13C: "itof0", 0x13D: "itof4", 0x13E: "itof12", 0x13F: "itof15", 0x17C: "ftoi0", 0x17D: "ftoi4", 0x17E: "ftoi12", 0x17F: "ftoi15",
         0x1BC: "mulax", 0x1BD: "mulay", 0x1BE: "mulaz", 0x1BF: "mulaw", 0x1FC: "mulaq", 0x1FD: "abs", 0x1FE: "mulai", 0x1FF: "clip",
         0x23C: "addaq", 0x23D: "maddaq", 0x23E: "addai", 0x23F: "maddai", 0x27C: "subaq", 0x27D: "msubaq", 0x27E: "subai", 0x27F: "msubai",
         0x2BC: "adda", 0x2BD: "madda", 0x2BE: "mula", 0x2FC: "suba", 0x2FD: "msuba", 0x2FE: "opmula", 0x2FF: "nop"}
    n = t.get(code11, f"upper2?{code11:#x}")
    if n == "nop":
        return "nop", flags
    if n == "clip":
        return f"clipw.xyz vf{fs}, vf{ft}w", flags
    if n.startswith(("itof", "ftoi", "abs")):
        return f"{n}.{d} vf{ft}, vf{fs}", flags
    if n[-1] in "xyzw" and n not in ("adda", "madda", "mula", "suba", "msuba"):
        return f"{n}.{d} ACC, vf{fs}, vf{ft}{n[-1]}", flags
    if n.endswith(("q", "i")):
        return f"{n}.{d} ACC, vf{fs}", flags
    return f"{n}.{d} ACC, vf{fs}, vf{ft}", flags


def lower(w, pc, labels):
    op = w >> 25
    it, is_, id_ = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    dest = FIELD((w >> 21) & 0xF)
    imm11 = w & 0x7FF
    if imm11 & 0x400:
        imm11 -= 0x800
    tgt = lambda: (pc + 8 + imm11 * 8)
    lab = lambda a: labels.get(a, f"{a:#x}")
    br = {0x20: "b", 0x21: "bal", 0x24: "jr", 0x25: "jalr", 0x28: "ibeq", 0x29: "ibne", 0x2C: "ibltz", 0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez"}
    if op == 0x40:
        f = w & 0x3F
        if f < 0x3C:
            names = {0x30: "iadd", 0x31: "isub", 0x32: "iaddi", 0x34: "iand", 0x35: "ior"}
            n = names.get(f, f"lower40?{f:#x}")
            if n == "iaddi":
                imm5 = (w >> 6) & 31
                imm5 = imm5 - 32 if imm5 & 16 else imm5
                return f"iaddi vi{it}, vi{is_}, {imm5}"
            return f"{n} vi{id_}, vi{is_}, vi{it}"
        code = w & 0x7FF
        t = {0x33C: "move", 0x33D: "mr32", 0x37C: "lqi", 0x37D: "sqi", 0x37E: "lqd", 0x37F: "sqd", 0x3BC: "div", 0x3BD: "sqrt", 0x3BE: "rsqrt",
             0x3BF: "waitq", 0x3FC: "mtir", 0x3FD: "mfir", 0x3FE: "ilwr", 0x3FF: "iswr", 0x43C: "rnext", 0x43D: "rget", 0x43E: "rinit", 0x43F: "rxor",
             0x6BC: "xtop", 0x6BD: "xitop", 0x6FC: "xgkick", 0x73C: "esadd", 0x73D: "ersadd", 0x73E: "eleng", 0x73F: "erleng", 0x77C: "eatanxy",
             0x77D: "eatanxz", 0x77E: "esum", 0x7BC: "esqrt", 0x7BD: "ersqrt", 0x7BE: "ercpr", 0x7BF: "waitp", 0x67C: "mfp", 0x7FC: "esin", 0x7FD: "eatan", 0x7FE: "eexp", 0x33F: "nop?"}
        n = t.get(code, f"lower40b?{code:#x}")
        fs, ft = is_, it
        fsf, ftf = "xyzw"[(w >> 21) & 3], "xyzw"[(w >> 23) & 3]
        if n in ("move", "mr32"):
            return f"{n}.{dest} vf{ft}, vf{fs}"
        if n == "lqi":
            return f"lqi.{dest} vf{ft}, (vi{is_}++)"
        if n == "sqi":
            return f"sqi.{dest} vf{fs}, (vi{it}++)"
        if n == "lqd":
            return f"lqd.{dest} vf{ft}, (--vi{is_})"
        if n == "sqd":
            return f"sqd.{dest} vf{fs}, (--vi{it})"
        if n in ("div",):
            return f"div Q, vf{fs}{fsf}, vf{ft}{ftf}"
        if n in ("sqrt", "rsqrt"):
            return f"{n} Q, vf{fs}{fsf}, vf{ft}{ftf}" if n == "rsqrt" else f"sqrt Q, vf{ft}{ftf}"
        if n == "mtir":
            return f"mtir vi{it}, vf{fs}{fsf}"
        if n == "mfir":
            return f"mfir.{dest} vf{ft}, vi{is_}"
        if n in ("ilwr", "iswr"):
            return f"{n}.{dest} vi{it}, (vi{is_})"
        if n in ("xtop", "xitop"):
            return f"{n} vi{it}"
        if n == "xgkick":
            return f"xgkick vi{is_}"
        if n == "mfp":
            return f"mfp.{dest} vf{ft}, P"
        if n.startswith("e"):
            return f"{n} P, vf{fs}"
        return n
    if op in br:
        n = br[op]
        if n in ("jr", "jalr"):
            return f"{n} vi{is_}" + (f", vi{it}" if n == "jalr" else "")
        if n in ("b",):
            return f"b {lab(tgt())}"
        if n == "bal":
            return f"bal vi{it}, {lab(tgt())}"
        if n in ("ibeq", "ibne"):
            return f"{n} vi{it}, vi{is_}, {lab(tgt())}"
        return f"{n} vi{is_}, {lab(tgt())}"
    imm15 = (w & 0x7FF) | ((w >> 10) & 0x7800)
    simm11 = w & 0x7FF
    simm11 = simm11 - 0x800 if simm11 & 0x400 else simm11
    m = {0x00: f"lq.{dest} vf{it}, {simm11}(vi{is_})", 0x01: f"sq.{dest} vf{is_}, {simm11}(vi{it})",
         0x04: f"ilw.{dest} vi{it}, {simm11}(vi{is_})", 0x05: f"isw.{dest} vi{it}, {simm11}(vi{is_})",
         0x08: f"iaddiu vi{it}, vi{is_}, {imm15:#x}", 0x09: f"isubiu vi{it}, vi{is_}, {imm15:#x}",
         0x10: f"fceq vi1, {w & 0xFFFFFF:#x}", 0x11: f"fcset {w & 0xFFFFFF:#x}", 0x12: f"fcand vi1, {w & 0xFFFFFF:#x}",
         0x13: f"fcor vi1, {w & 0xFFFFFF:#x}", 0x14: f"fseq vi{it}, {w & 0xFFF:#x}", 0x15: f"fsset {w & 0xFFF:#x}",
         0x16: f"fsand vi{it}, {w & 0xFFF:#x}", 0x17: f"fsor vi{it}, {w & 0xFFF:#x}", 0x18: f"fmeq vi{it}, vi{is_}",
         0x1A: f"fmand vi{it}, vi{is_}", 0x1B: f"fmor vi{it}, vi{is_}", 0x1C: f"fcget vi{it}", 0x02: f"lower02?"}
    return m.get(op, f"lower?{op:#x}")


def main():
    path, off, size = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
    labels = {}
    if len(sys.argv) > 4:
        for kv in sys.argv[4].split(","):
            a, n = kv.split("=")
            labels[int(a, 0)] = n
    data = open(path, "rb").read()[off:off + size]
    for i in range(0, len(data) - 7, 8):
        lo, hi = struct.unpack_from("<II", data, i)
        pc = i
        if pc in labels:
            print(f"{labels[pc]}:")
        u, flags = upper(hi)
        if hi & (1 << 31):  # I bit: lower word is a float immediate for the I register
            l = f"loi {struct.unpack('<f', struct.pack('<I', lo))[0]}"
        else:
            l = lower(lo, pc, labels)
        print(f"  {pc:04x}: {u:34} {l:36} {('['+flags+']') if flags else ''}")


if __name__ == "__main__":
    main()
