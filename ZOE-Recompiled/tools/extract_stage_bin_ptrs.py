"""List the main-ELF code addresses that Zone of the Enders' per-stage <stage>.bin files point at.

Each stage folder in ZOE.DAT holds a ~3 KB <stage>.bin: MIPS code that the game loads at run time and that
registers (hash, function pointer) pairs through `jal 0x43bb90` with `lui a1,hi ; addiu a1,a1,lo` address
pairs. Those main-ELF functions (and the jal targets 0x43bb90 / 0x43c8e8 themselves) are referenced from
nowhere inside SLUS_201.48 itself, so Ghidra's flow analysis never finds them; CreateMissingFunctions.java reads the CSV written here as one of its
pointer sources.

usage: extract_stage_bin_ptrs.py [--iso PATH] [--elf PATH] [--out CSV]
Defaults come from ZOE-Recompiled/zoe.cfg (CD_IMAGE, ELF) and write work/names/zoe_stage_bin_ptrs.csv.

ZOE.DAT layout (found by inspection, all little endian): a table of 32-byte entries
  name[16] | u32 a | u32 b | u32 flags | u32 0
Files have flags 0 with a = first sector (2048-byte, relative to the start of ZOE.DAT) and b = size in
bytes. Directories have flags 0x102: either a = entry count and b = sector of the sub-table, or (as the
"stage" entry does) a = b = 0 and the count / byte offset of the children inside the *same* table stored
in bytes 8..11 / 12..15 of the name field.
"""
import argparse
import os
import struct
import sys

SECTOR = 2048
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # ZOE-Recompiled


def read_cfg(path):
    cfg = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            cfg[k] = v
    return cfg


def find_zoe_dat(iso):
    iso.seek(16 * SECTOR)
    pvd = iso.read(SECTOR)
    lba, size = struct.unpack_from("<II", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]

    def walk(lba, size):
        iso.seek(lba * SECTOR)
        data = iso.read(size)
        pos = 0
        while pos < len(data):
            rl = data[pos]
            if rl == 0:
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = data[pos:pos + rl]
            pos += rl
            el, es = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            nl = rec[32]
            name = rec[33:33 + nl].decode("ascii", "replace").split(";")[0]
            if rec[25] & 2:
                if name not in ("\x00", "\x01"):
                    r = walk(el, es)
                    if r:
                        return r
            elif name == "ZOE.DAT":
                return el
        return None

    return walk(lba, size)


def read_table(iso, base, sector, count, byte_off=0):
    iso.seek(base + sector * SECTOR + byte_off)
    raw = iso.read(32 * count)
    out = []
    for i in range(count):
        e = raw[i * 32:i * 32 + 32]
        name = e[:16].split(b"\0")[0].decode("latin1")
        a, b, flags = struct.unpack_from("<III", e, 16)
        out.append((name, a, b, flags, struct.unpack_from("<II", e, 8)))
    return out


def list_files(iso, base):
    files = []

    def visit(prefix, table, table_sector):
        consumed = set()  # entries that are already the children of an inline directory ("stage")
        for idx, (name, a, b, flags, inl) in enumerate(table):
            if not name or idx in consumed:
                continue
            if flags == 0x102:
                if a == 0 and b == 0 and inl[0]:
                    first = inl[1] // 32
                    consumed.update(range(first, first + inl[0]))
                    visit(prefix + name + "/", table[first:first + inl[0]], table_sector)
                elif a:
                    visit(prefix + name + "/", read_table(iso, base, b, a), b)
            else:
                files.append((prefix + name, a, b))

    top = read_table(iso, base, 0, 26)
    visit("", top, 0)
    return files


def text_range(elf_path):
    d = open(elf_path, "rb").read()
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    stroff = secs[shstrndx][4]
    for name, typ, flags, addr, off, size, *_ in secs:
        n = d[stroff + name:d.index(b"\0", stroff + name)].decode()
        if n == ".text":
            return addr, addr + size
    raise SystemExit("no .text section in " + elf_path)


def main():
    cfg = read_cfg(os.path.join(ROOT, "zoe.cfg"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=cfg.get("CD_IMAGE"))
    ap.add_argument("--elf", default=cfg.get("ELF"))
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "names", "zoe_stage_bin_ptrs.csv"))
    args = ap.parse_args()
    tb, te = text_range(args.elf)

    with open(args.iso, "rb") as iso:
        lba = find_zoe_dat(iso)
        if lba is None:
            raise SystemExit("ZOE.DAT not found in " + args.iso)
        base = lba * SECTOR
        files = list_files(iso, base)
        bins = [f for f in files if f[0].endswith(".bin")]
        print(f"ZOE.DAT at LBA {lba:#x}: {len(files)} files, {len(bins)} .bin", file=sys.stderr)
        found = {}  # address -> (file, offset)
        for name, sector, size in bins:
            iso.seek(base + sector * SECTOR)
            d = iso.read(size)
            w = struct.unpack("<%dI" % (len(d) // 4), d[:len(d) // 4 * 4])
            for i, x in enumerate(w):
                if tb <= x < te and x % 4 == 0:  # raw pointer word
                    found.setdefault(x, (name, i * 4, "word"))
                if x >> 26 in (0x02, 0x03):  # j / jal straight into the ELF (register-callback helpers)
                    t = (x & 0x3FFFFFF) << 2
                    if tb <= t < te:
                        found.setdefault(t, (name, i * 4, "jal" if x >> 26 == 3 else "j"))
                    continue
                if x >> 26 != 0x0F:  # lui
                    continue
                rt = (x >> 16) & 31
                for la in range(1, 5):
                    if i + la >= len(w):
                        break
                    y = w[i + la]
                    op = y >> 26
                    if op in (0x09, 0x0D) and ((y >> 21) & 31) == rt:
                        lo = y & 0xFFFF
                        t = ((x & 0xFFFF) << 16) + (lo if op == 0x0D else (lo - 0x10000 if lo & 0x8000 else lo))
                        t &= 0xFFFFFFFF
                        if tb <= t < te and t % 4 == 0:
                            found.setdefault(t, (name, (i + la) * 4, "lui"))
                        break

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", newline="") as f:
        f.write("address,source_file,offset,kind\n")
        for a in sorted(found):
            n, off, kind = found[a]
            f.write(f"0x{a:08X},{n},0x{off:X},{kind}\n")
    print(f"{len(found)} distinct .text addresses from {len(bins)} stage .bin files -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
