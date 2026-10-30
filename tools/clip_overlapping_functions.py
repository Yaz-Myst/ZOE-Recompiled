"""Clip Ghidra-exported function ranges whose span far exceeds their real size.

Ghidra gives a function a discontiguous body when a branch leaves for a far shared tail. ExportPS2Functions writes
Start = min, End = max of that body, so the translator (which uses Start/End verbatim) sees one function spanning
everything in between. Seen in practice: three such "functions" of 1-2 MB produced 90-164 MB of C++ each (MSVC C1025 /
C1128). This sets End = Start + Size (the body's byte count) where the span exceeds Size by more than --gap bytes
(default 4096; ZOE runs fine with gaps up to 3.9 KB). Keeps <map>.orig once.
usage: python clip_overlapping_functions.py <map.csv> [--gap N] [--apply]
"""
import csv
import os
import shutil
import sys


def main():
    path = sys.argv[1]
    apply = '--apply' in sys.argv
    gap = int(sys.argv[sys.argv.index('--gap') + 1]) if '--gap' in sys.argv else 4096
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        fields = reader.fieldnames
        rows = list(reader)
    clipped = []
    for r in rows:
        s, e, z = int(r['Start'], 16), int(r['End'], 16), int(r['Size'])
        if e - s - z > gap:
            clipped.append((r['Name'], s, e, s + z))
            r['End'] = '0x%08X' % (s + z)
    print('%d of %d functions clipped (gap > %d)' % (len(clipped), len(rows), gap))
    for n, s, e, ne in sorted(clipped, key=lambda c: c[3] - c[2]):
        print('  %-22s 0x%06X: end 0x%06X -> 0x%06X (-%d bytes)' % (n, s, e, ne, e - ne))
    if not apply:
        print('(dry run; add --apply to rewrite the map)')
        return
    if not os.path.exists(path + '.orig'):
        shutil.copyfile(path, path + '.orig')
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields, lineterminator='\n')
        w.writeheader()
        w.writerows(rows)
    print('rewrote', path)


if __name__ == '__main__':
    main()
