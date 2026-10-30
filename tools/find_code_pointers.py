"""Find function entry points that are only reached through pointers (callback tables,
`la reg, func` + jalr) and are missing from a Ghidra function map.

usage: find_code_pointers.py <module.elf> <module_map.csv> [--verbose]

Prints one address per line (0x...) for targets that
  * are referenced by a 32-bit word in a data section, or built by a lui/addiu(ori) pair in code,
  * land on an instruction in an executable section,
  * look like a function start: `addiu $sp,$sp,-N` at the target, or directly after `jr $ra` + delay slot,
  * are not already the start of a record in the map.
"""
import struct
import sys


def read_elf(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        raise SystemExit(f'{path}: not an ELF')
    e_shoff, = struct.unpack_from('<I', d, 0x20)
    e_shentsize, e_shnum = struct.unpack_from('<HH', d, 0x2E)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (name, stype, flags, addr, offset, size) = struct.unpack_from('<IIIIII', d, off)
        if stype == 8 or size == 0 or addr == 0:  # NOBITS / non-alloc
            continue
        if not flags & 0x2:  # SHF_ALLOC
            continue
        sections.append({'addr': addr, 'size': size, 'exec': bool(flags & 0x4), 'data': d[offset:offset + size]})
    return sections


def read_map(path):
    starts = set()
    for line in open(path, encoding='utf-8', errors='replace'):
        parts = line.strip().split(',')
        if len(parts) >= 2 and parts[1].startswith('0x'):
            try:
                starts.add(int(parts[1], 16))
            except ValueError:
                pass
    return starts


def main():
    elf, csv = sys.argv[1], sys.argv[2]
    verbose = '--verbose' in sys.argv
    sections = read_elf(elf)
    starts = read_map(csv)
    code = [s for s in sections if s['exec']]

    def word_at(addr):
        for s in code:
            if s['addr'] <= addr < s['addr'] + s['size'] - 3:
                return struct.unpack_from('<I', s['data'], addr - s['addr'])[0]
        return None

    def in_code(addr):
        return addr % 4 == 0 and word_at(addr) is not None

    def looks_like_function_start(addr):
        w = word_at(addr)
        if w is None:
            return False
        if (w & 0xFFFF0000) == 0x27BD0000 and (w & 0x8000):  # addiu sp, sp, -N
            return True
        prev2 = word_at(addr - 8)
        return prev2 == 0x03E00008  # jr ra ; <delay slot> ; <target>

    def table_neighbours(target):
        # A data word pointing into code that sits among other code pointers (a table), not a lone
        # value that happens to fall in the code range.
        for s in sections:
            for off in range(0, s['size'] - 3, 4):
                if struct.unpack_from('<I', s['data'], off)[0] != target:
                    continue
                near = 0
                for d in (-16, -12, -8, -4, 4, 8, 12, 16):
                    o = off + d
                    if 0 <= o <= s['size'] - 4 and in_code(struct.unpack_from('<I', s['data'], o)[0]):
                        near += 1
                if near >= 1:
                    return True
        return False

    candidates = {}
    table_targets = set()  # stored as data: callback tables are trusted without the prologue test
    # 1. pointer-sized words anywhere (overlay images are a single RWX section holding code and data)
    for s in sections:
        for off in range(0, s['size'] - 3, 4):
            v = struct.unpack_from('<I', s['data'], off)[0]
            if in_code(v):
                candidates.setdefault(v, f'data@0x{s["addr"] + off:x}')
                table_targets.add(v)
    # 2. lui/addiu (or ori) address construction in code
    for s in code:
        n = s['size'] // 4
        words = struct.unpack_from(f'<{n}I', s['data'], 0)
        for i, w in enumerate(words):
            if (w >> 26) != 0x0F:  # lui
                continue
            rt = (w >> 16) & 31
            hi = (w & 0xFFFF) << 16
            for j in range(i + 1, min(n, i + 12)):
                w2 = words[j]
                op = w2 >> 26
                if op in (0x09, 0x0D) and ((w2 >> 21) & 31) == rt:  # addiu / ori rX, rt, lo
                    lo = w2 & 0xFFFF
                    addr = (hi + (lo - 0x10000 if (op == 0x09 and lo & 0x8000) else lo)) & 0xFFFFFFFF
                    if in_code(addr):
                        candidates.setdefault(addr, f'la@0x{s["addr"] + 4 * j:x}')
                    break
                if ((w2 >> 16) & 31) == rt and op not in (0x2B, 0x29, 0x28, 0x3F):  # rt overwritten (not a store)
                    break

    found = sorted(a for a in candidates
                   if a not in starts and (looks_like_function_start(a) or
                                           (a in table_targets and table_neighbours(a))))
    for a in found:
        print(f'0x{a:x}' + (f'  # {candidates[a]}' if verbose else ''))
    print(f'# {len(found)} missing entry point(s); {len(candidates)} code pointer(s) total', file=sys.stderr)


if __name__ == '__main__':
    main()
