"""Stub review gate, run by build.ps1 before every build of a game whose config sets STUB_REVIEW.

The translator replaces a function with a runtime stand-in (a generated file that only calls ps2_stubs::X or
ps2_syscalls::X) because of the function's NAME. In a stripped game those names come from automatic matching
against Sony's libraries, and a wrong match silently deletes game code: ZOE's ground probe called a 7-instruction
game routine at 0x446010 that a 0.71 signature match had named scePadExitPressMode, so the floor collision and
scenery destruction quietly stopped working. Nothing crashed and nothing was logged.

So every stand-in must be listed in the game's reviewed file, with the reason it is really that library function.
The build fails, listing the functions, when:
  - a generated stand-in is not in the reviewed file (a new name match, a Ghidra re-export, a lost recompile entry);
  - a reviewed stand-in lies below LIB_START (the game's own code) and its reason does not start with "game code:".
To fix: disassemble the function and compare it with the real library code. If it is that function, add a line to
the reviewed file saying why. If not, or unsure, put its name in the TOML's `recompile` list so the game's own
code is translated (and correct the name source, e.g. work/names/*.csv).

Reviewed file: CSV "address,name,reason" (address in hex), '#' lines are comments.
usage: python check_stubs.py <reviewed.csv> <LIB_START hex> <generated dir> [more generated dirs ...]
exit code: 0 = every stand-in reviewed, 1 = problems listed, 2 = bad input
"""
import csv
import os
import re
import sys

WRAPPER = re.compile(r'void (\w+?)_0x([0-9a-fA-F]+)\(uint8_t\* rdram, R5900Context\* ctx, PS2Runtime \*runtime\) \{(.*?)\n\}', re.S)
CALL = re.compile(r'ctx->pc = getRegU32\(ctx, 31\);\s*(ps2_stubs|ps2_syscalls)::(\w+)\(rdram, ctx, runtime\);')


def stand_ins(dirs):
    found = {}
    for d in dirs:
        for name in os.listdir(d):
            if not name.endswith('.cpp'):
                continue
            path = os.path.join(d, name)
            text = open(path, encoding='utf-8', errors='ignore').read()
            if 'switch (ctx->pc)' in text:   # translated game code, not a stand-in
                continue
            for m in WRAPPER.finditer(text):
                c = CALL.search(m.group(3))
                if c:
                    found[int(m.group(2), 16)] = (m.group(1), '%s::%s' % (c.group(1), c.group(2)), path)
    return found


def reviewed(path):
    out = {}
    with open(path, encoding='utf-8') as f:
        rows = csv.reader(line for line in f if line.strip() and not line.lstrip().startswith('#'))
        for row in rows:
            if row and row[0].strip().lower() == 'address':
                continue
            if len(row) < 3 or not row[2].strip():
                raise ValueError('%s: want "address,name,reason": %s' % (path, ','.join(row)))
            out[int(row[0], 16)] = (row[1].strip(), ','.join(row[2:]).strip())
    return out


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    review_path, lib_start, dirs = sys.argv[1], int(sys.argv[2], 16), sys.argv[3:]
    try:
        ok = reviewed(review_path)
    except (OSError, ValueError) as e:
        print('  stub review: cannot read %s: %s' % (review_path, e))
        return 2
    found = stand_ins(dirs)
    problems = []
    for addr in sorted(found):
        name, target, path = found[addr]
        entry = ok.get(addr)
        if entry is None or entry[0] != name:
            why = 'not reviewed' if entry is None else 'reviewed as %s, now named %s' % (entry[0], name)
            where = 'GAME CODE (below LIB_START 0x%x)' % lib_start if addr < lib_start else 'library area'
            problems.append('0x%08x %-28s -> %-34s %s; %s' % (addr, name, target, why, where))
        elif addr < lib_start and not entry[1].lower().startswith('game code:'):
            problems.append('0x%08x %-28s -> %-34s in GAME CODE (below LIB_START 0x%x); its reason must start with '
                            '"game code:" and say why replacing it is safe' % (addr, name, target, lib_start))
    stale = sorted(a for a in ok if a not in found)
    print('  stub review: %d stand-ins, %d reviewed entries, %d problem(s)%s' % (
        len(found), len(ok), len(problems), (', %d reviewed entries no longer stand-ins' % len(stale)) if stale else ''))
    if problems:
        print('  These functions would be REPLACED by runtime stand-ins without a review (%s):' % review_path)
        for p in problems:
            print('    ' + p)
        print('  For each: disassemble it and compare with the real library function. If it is that function, add')
        print('  "address,name,reason" to the reviewed file. If not (or unsure), add its name to the TOML `recompile`')
        print('  list so the game\'s own code runs, and fix the name source. Never accept a name match unchecked.')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
