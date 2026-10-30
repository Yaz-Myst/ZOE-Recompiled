"""Mechanical evidence for a stand-in review: compare each stubbed function of a stripped ELF with the same-named
function of a reference ELF that has symbols (any PS2 title linked with the same SDK libraries and not stripped).

For every "name@0xADDR" in the target TOML's stubs list:
  * kernel wrapper (addiu v1,zero,N ; syscall): the syscall number N must equal the reference's -> "syscall N"
  * otherwise the first 24 instruction words are compared with relocation fields masked (j/jal targets, lui/addiu
    immediates); the match ratio is reported.
usage: python stub_evidence.py <target.toml> <target ELF> <ref ELF> <out.csv>
out.csv: address,name,kind,evidence   (kind: syscall-ok | syscall-DIFF | sim=<ratio> | no-ref)
"""
import re
import struct
import sys

WORDS = 24


def load_elf(path):
    d = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
    sh = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
    secs = [(s[3], s[4], s[5], s[1]) for s in sh if s[1] in (1,) and s[3]]   # addr, off, size, type
    syms = {}
    for s in sh:
        if s[1] == 2:  # SYMTAB
            strtab = sh[s[6]]
            for i in range(s[5] // 16):
                nm, val, size, info = struct.unpack_from('<IIIB', d, s[4] + i * 16)
                if info & 0xf == 2 and val:
                    end = d.index(b'\0', strtab[4] + nm)
                    syms.setdefault(d[strtab[4] + nm:end].decode(), val)
    return d, secs, syms


def words(elf, addr, n=WORDS):
    d, secs, _ = elf
    for a, off, size, _t in secs:
        if a <= addr < a + size:
            o = off + (addr - a)
            k = min(n, (a + size - addr) // 4)
            return list(struct.unpack_from('<%dI' % k, d, o))
    return []


def mask(w):
    op = w >> 26
    if op in (2, 3):                      # j / jal
        return w & 0xfc000000
    if op == 0xf:                         # lui
        return w & 0xffff0000 if 0x1 <= (w & 0xffff) <= 0x1ff else w
    if op in (8, 9, 0xd, 0x18, 0x19) or 0x20 <= op <= 0x2f or 0x31 <= op <= 0x3f:  # imm arith, loads/stores
        return w & 0xffff0000
    return w


def syscall_no(ws):
    # addiu v1,zero,imm (0x2403xxxx) followed by syscall (0x0000000c) within the first 3 words
    for i, w in enumerate(ws[:3]):
        if (w & 0xffff0000) == 0x24030000 and len(ws) > i + 1 and ws[i + 1] == 0x0000000c:
            imm = w & 0xffff
            return imm - 0x10000 if imm & 0x8000 else imm
    return None


def main():
    toml, tgt_path, ref_path, out = sys.argv[1:5]
    tgt = load_elf(tgt_path)
    ref = load_elf(ref_path)
    body = re.search(r'^stubs = \[(.*?)^\]', open(toml).read(), re.S | re.M).group(1)
    rows = []
    for name, addr in re.findall(r'"(\w+)@0x([0-9A-Fa-f]+)"', body):
        a = int(addr, 16)
        tw = words(tgt, a)
        tn = syscall_no(tw)
        ra = ref[2].get(name) or ref[2].get('_' + name)
        if tn is not None:
            rn = syscall_no(words(ref, ra)) if ra else None
            kind = 'syscall-ok' if rn == tn else 'syscall-DIFF'
            ev = 'kernel wrapper: syscall %d (reference %s)' % (tn, rn)
        elif ra:
            rw = words(ref, ra)
            n = min(len(tw), len(rw))
            same = sum(1 for i in range(n) if mask(tw[i]) == mask(rw[i]))
            kind = 'sim=%.2f' % (same / n if n else 0)
            ev = 'first %d masked words: %d/%d equal to reference %s@0x%06X' % (n, same, n, name, ra)
        else:
            kind = 'no-ref'
            ev = 'no function of that name in the reference ELF'
        rows.append((a, name, kind, ev))
    with open(out, 'w', newline='') as f:
        f.write('address,name,kind,evidence\n')
        for a, name, kind, ev in rows:
            f.write('0x%08X,%s,%s,"%s"\n' % (a, name, kind, ev))
    from collections import Counter
    print(Counter(k.split('=')[0] for _, _, k, _ in rows))
    for a, name, kind, ev in rows:
        if kind == 'syscall-DIFF' or kind == 'no-ref' or (kind.startswith('sim=') and float(kind[4:]) < 0.85):
            print(' 0x%06X %-26s %-12s %s' % (a, name, kind, ev))


if __name__ == '__main__':
    main()
