"""Clip implausibly large Ghidra "functions" in an overlay map CSV.

Raw overlays put their data after the code, and Ghidra sometimes marks a label near the
code/data boundary as executable, producing a single "function" that spans the entire data
section. Such rows are clipped to end after their first `jr $ra` + delay slot, or dropped
when the range contains no return at all, so the recompiler does not translate hundreds of
KB of data as instructions.

usage: trim_data_functions.py <overlay.bin> <map.csv> [max_size=0x8000] [base=0x500000]
The CSV is rewritten in place; a .orig copy is kept.
"""
import csv
import os
import shutil
import struct
import sys

JR_RA = 0x03E00008


def main():
    bin_path, csv_path = sys.argv[1:3]
    max_size = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x8000
    base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x500000
    blob = open(bin_path, "rb").read()

    with open(csv_path, newline="") as f:
        rows = list(csv.DictReader(f))
    fields = ["Name", "Start", "End", "Size"]

    kept = []
    for row in rows:
        kept.append(row)
        start, end = int(row["Start"], 16), int(row["End"], 16)
        if end - start <= max_size:
            continue
        new_end = None
        for addr in range(start, end, 4):
            off = addr - base
            if off + 4 > len(blob):
                break
            if struct.unpack_from("<I", blob, off)[0] == JR_RA:
                new_end = addr + 8
                break
        if new_end is None:
            print(f"{row['Name']}: {start:#x}-{end:#x} ({end - start:#x}) -> dropped (no return)")
            kept.pop()
            continue
        print(f"{row['Name']}: {start:#x}-{end:#x} ({end - start:#x}) -> {start:#x}-{new_end:#x}")
        row["End"] = f"0x{new_end:08X}"
        row["Size"] = str(new_end - start)

    rows = kept
    if not os.path.exists(csv_path + ".orig"):
        shutil.copyfile(csv_path, csv_path + ".orig")
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


if __name__ == "__main__":
    main()
