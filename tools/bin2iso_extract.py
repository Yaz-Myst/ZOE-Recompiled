"""Convert a MODE2/2352 PS2 CD BIN to a 2048-byte ISO and extract its ISO9660 file tree."""
import os
import struct
import sys

SECTOR_RAW = 2352
SECTOR = 2048
DATA_OFF = 24  # Mode 2 Form 1: 12 sync + 4 header + 8 subheader


def bin_to_iso(bin_path, iso_path):
    with open(bin_path, "rb") as src, open(iso_path, "wb") as dst:
        while True:
            raw = src.read(SECTOR_RAW)
            if len(raw) < SECTOR_RAW:
                break
            dst.write(raw[DATA_OFF:DATA_OFF + SECTOR])


def walk(iso, lba, size, out_dir, listing, prefix=""):
    iso.seek(lba * SECTOR)
    data = iso.read(size)
    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:  # records do not cross sector boundaries
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + rec_len]
        pos += rec_len
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len]
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace").split(";")[0]
        rel = prefix + name
        if flags & 2:
            os.makedirs(os.path.join(out_dir, rel), exist_ok=True)
            walk(iso, ext_lba, ext_size, out_dir, listing, rel + "/")
        else:
            listing.append((rel, ext_lba, ext_size))
            here = iso.tell()
            iso.seek(ext_lba * SECTOR)
            with open(os.path.join(out_dir, rel), "wb") as f:
                remaining = ext_size
                while remaining:
                    chunk = iso.read(min(remaining, 1 << 20))
                    f.write(chunk)
                    remaining -= len(chunk)
            iso.seek(here)


def extract(iso_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    listing = []
    with open(iso_path, "rb") as iso:
        iso.seek(16 * SECTOR)
        pvd = iso.read(SECTOR)
        assert pvd[1:6] == b"CD001", "no ISO9660 PVD"
        root = pvd[156:156 + 34]
        walk(iso, struct.unpack_from("<I", root, 2)[0],
             struct.unpack_from("<I", root, 10)[0], out_dir, listing)
    with open(os.path.join(out_dir, "..", "filelist.txt"), "w") as f:
        for rel, lba, size in listing:
            f.write(f"{lba:8d} {size:10d} {rel}\n")
    return listing


if __name__ == "__main__":
    bin_path, iso_path, out_dir = sys.argv[1:4]
    if not os.path.exists(iso_path):
        bin_to_iso(bin_path, iso_path)
    for rel, lba, size in extract(iso_path, out_dir):
        print(f"{size:10d} {rel}")
