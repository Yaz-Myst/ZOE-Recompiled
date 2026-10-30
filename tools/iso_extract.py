#!/usr/bin/env python3
"""Extract one file (or list the tree) from a PlayStation 2 ISO 9660 image.

The game program is user-supplied copyrighted data. This tool only copies it out of the
user's own image into the git-ignored ZOE-Recompiled/game/ folder; never commit the result.

usage:
  python3 tools/iso_extract.py IMAGE.iso --list
  python3 tools/iso_extract.py IMAGE.iso --file SLUS_201.48 --out ZOE-Recompiled/game
"""
import argparse
import os
import struct
import sys
import zlib

SECTOR = 2048


def records(f, lba, size):
    f.seek(lba * SECTOR)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:  # records never cross a sector
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + length]
        pos += length
        if len(rec) < 34 or rec[0] != length:
            raise ValueError("damaged ISO 9660 directory record")
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len]
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace").split(";")[0].rstrip(".")
        yield name, ext_lba, ext_size, bool(flags & 2)


def walk(f, lba, size, prefix=""):
    for name, ext_lba, ext_size, directory in records(f, lba, size):
        path = prefix + name
        if directory:
            yield from walk(f, ext_lba, ext_size, path + "/")
        else:
            yield path, ext_lba, ext_size


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("--file", help="file to extract (case-insensitive, ISO name)")
    ap.add_argument("--out", default="ZOE-Recompiled/game", help="output directory")
    ap.add_argument("--list", action="store_true", help="list files instead of extracting")
    ap.add_argument("--size", type=int, help="expected size in bytes (verification)")
    ap.add_argument("--crc32", help="expected CRC-32 in hex (verification)")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            sys.exit("not an ISO 9660 image with 2048-byte sectors (convert .bin first)")
        root = pvd[156:156 + 34]
        root_lba = struct.unpack_from("<I", root, 2)[0]
        root_size = struct.unpack_from("<I", root, 10)[0]
        files = list(walk(f, root_lba, root_size))

        if args.list:
            for path, _, size in files:
                print(f"{size:12d}  {path}")
            return

        if not args.file:
            sys.exit("--file is required unless --list is used")
        wanted = args.file.replace("\\", "/").lstrip("/").upper()
        match = next((item for item in files if item[0].upper() == wanted or item[0].upper().endswith("/" + wanted)), None)
        if match is None:
            sys.exit(f"{args.file} not found in {args.image}")
        path, lba, size = match
        os.makedirs(args.out, exist_ok=True)
        target = os.path.join(args.out, os.path.basename(path))
        f.seek(lba * SECTOR)
        data = f.read(size)
        with open(target, "wb") as out:
            out.write(data)

        crc = zlib.crc32(data) & 0xFFFFFFFF
        print(f"extracted {path}: {size} bytes, CRC-32 {crc:08X} -> {target}")
        if args.size is not None and size != args.size:
            sys.exit(f"size mismatch: expected {args.size}, got {size}")
        if args.crc32 is not None and crc != int(args.crc32, 16):
            sys.exit(f"CRC-32 mismatch: expected {args.crc32}, got {crc:08X}")


if __name__ == "__main__":
    main()
