"""Find code addresses an overlay references that Ghidra did not mark as functions.

Looks at every JAL target and every lui/addiu (or lui/ori) constant pair that lands inside
the overlay on a stack-frame prologue (addiu $sp,$sp,-N). Such targets are callbacks the
game passes around by pointer; if they are not function starts the runtime cannot
dispatch to them.

usage: overlay_coverage.py <overlay.bin> <map.csv> [base=0x500000]
Prints missing addresses and whether each falls inside a known function or in a gap.
"""
import csv
import struct
import sys


def main():
    bin_path, csv_path = sys.argv[1:3]
    base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x500000
    blob = open(bin_path, "rb").read()
    end = base + len(blob)
    words = struct.unpack_from(f"<{len(blob) // 4}I", blob)

    def word_at(addr):
        return words[(addr - base) >> 2]

    def is_prologue(addr):
        if not (base <= addr < end) or addr & 3:
            return False
        w = word_at(addr)
        return (w >> 16) == 0x27BD and (w & 0x8000)

    funcs = []
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            funcs.append((int(row["Start"], 16), int(row["End"], 16), row["Name"]))
    starts = {s for s, _, _ in funcs}

    refs = {}
    for i, w in enumerate(words):
        pc = base + 4 * i
        op = w >> 26
        if op == 3:  # jal
            t = (w & 0x3FFFFFF) << 2
            if base <= t < end:
                refs.setdefault(t, set()).add(f"jal@{pc:#x}")
        elif op == 0xF:  # lui rt, hi
            rt, hi = (w >> 16) & 31, w & 0xFFFF
            for j in range(1, 6):  # the low half usually follows within a few insns
                if i + j >= len(words):
                    break
                w2 = words[i + j]
                op2, rs2 = w2 >> 26, (w2 >> 21) & 31
                if rs2 != rt or op2 not in (9, 0xD):
                    continue
                lo = w2 & 0xFFFF
                t = (hi << 16) + (lo - 0x10000 if op2 == 9 and lo & 0x8000 else lo)
                if is_prologue(t):
                    refs.setdefault(t, set()).add(f"ptr@{pc:#x}")
                break

    missing = sorted(t for t in refs if t not in starts)
    for t in missing:
        owner = next((n for s, e, n in funcs if s < t < e), None)
        where = f"inside {owner}" if owner else "gap"
        print(f"{t:#x} {where:40} {','.join(sorted(refs[t]))[:80]}")
    print(f"# {len(missing)} missing of {len(refs)} referenced code addresses, {len(funcs)} functions")


if __name__ == "__main__":
    main()
