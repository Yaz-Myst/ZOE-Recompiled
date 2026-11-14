#!/usr/bin/env python3
"""Translate IOP (R3000) module code from a save state's IOP RAM into C++ blocks for PS2X_IOP_AOT.

usage: iop_aotgen.py --state <file.p2s> --irx-dir <game/MODULES> --out <file.cpp> --entry <registerFunctionName>
                     [--modules IOP_MAIN,LIBSD,...]

The code is taken from RAM (already relocated), for the .text range of every loaded module whose .IRX is found in
--irx-dir. One C++ function per basic block; each does exactly what IopCpuCore::executeWord does for the same
instructions (ps2xIOP/src/emulator/core/iop_cpu.cpp), including the load delay, and returns before anything that
could raise an exception. The runtime only runs a block while RAM still holds these exact words (iop_aot.h).
Check a new translation with the offline replay: ZOE-Recompiled/tools/iop_replay (state bytes equal with and without).
"""
import argparse
import os
import re
import struct

RAM_SIZE = 0x200000
MAX_BLOCK = 48
M32 = 0xFFFFFFFF
IOPS_TAG = b'IOPS' + bytes([1, 0, 0, 0])


def find_ram(d):
    tag = d.find(b'IOPS\x01\x00\x00\x00')
    if tag < 0:
        raise SystemExit('no IOPS section in the state')
    at = d.find(struct.pack('<Q', RAM_SIZE), tag)
    if at < 0 or at - tag > 0x10000:
        raise SystemExit('IOP RAM not found after the IOPS tag')
    return d[at + 8:at + 8 + RAM_SIZE]


def find_modules(d):
    """(name, base) of the loaded modules: the state's module list is id, path, name, base, size, ... per module."""
    out = {}
    for m in re.finditer(rb'[\x05-\x40]\x00\x00\x00([ -~]{5,64}?)([\x05-\x40])\x00\x00\x00', d):
        path, nlen = m.group(1), m.group(2)[0]
        if d[m.start()] != len(path):
            continue
        name = d[m.end():m.end() + nlen]
        if len(name) != nlen or not re.fullmatch(rb'[ -~]+', name) or not path.endswith(name):
            continue
        base, size = struct.unpack_from('<II', d, m.end() + nlen)
        if base < RAM_SIZE and 0 < size <= RAM_SIZE - base:
            out[name.decode().split(';')[0].upper()] = base
    return out


def irx_text(path):
    e = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', e, 0x20)
    shentsize, shnum, shstr = struct.unpack_from('<HHH', e, 0x2E)
    secs = [struct.unpack_from('<10I', e, shoff + shentsize * i) for i in range(shnum)]
    for s in secs:
        if e[secs[shstr][4] + s[0]:].split(b'\0')[0] == b'.text':
            return s[3], s[5]  # address relative to the module base, size
    return None


def is_branch(w):
    op, rt, fn = w >> 26, (w >> 16) & 31, w & 63
    return (op == 0 and fn in (8, 9)) or (op == 1 and rt in (0, 1, 0x10, 0x11)) or 2 <= op <= 7


SPECIAL_OK = {0, 2, 3, 4, 6, 7, 8, 9, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B,
              0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B}
OPS_OK = {2, 3, 4, 5, 6, 7, 8, 9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF,
          0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E}


def is_bad(w, nxt):
    """Left to the interpreter: exceptions by design, COP0, unknown encodings, import stubs (jr ra + li $0)."""
    op, rt, fn = w >> 26, (w >> 16) & 31, w & 63
    if w == 0x03E00008 and (nxt & 0xFFFF0000) == 0x24000000:
        return True
    if op == 0:
        return fn not in SPECIAL_OK
    if op == 1:
        return rt not in (0, 1, 0x10, 0x11)
    return op not in OPS_OK


class Gen:
    def __init__(self, ram):
        self.ram = ram

    def word(self, pc):
        return struct.unpack_from('<I', self.ram, pc)[0]

    def blocks(self, lo, hi):
        leaders = {lo}
        for pc in range(lo, hi, 4):
            w = self.word(pc)
            nxt = self.word(pc + 4) if pc + 4 < hi else 0
            if is_bad(w, nxt):
                leaders.add(pc + 4)
            elif is_branch(w):
                leaders.add(pc + 8)
                op = w >> 26
                if op in (2, 3):
                    leaders.add(((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
                elif op != 0:
                    simm = (w & 0xFFFF) - (0x10000 if w & 0x8000 else 0)
                    leaders.add((pc + 4 + (simm << 2)) & M32)
        out = []
        todo = sorted(x for x in leaders if lo <= x < hi)
        while todo:
            start = todo.pop(0)
            pcs, pc = [], start
            while pc < hi and len(pcs) < MAX_BLOCK:
                w = self.word(pc)
                nxt = self.word(pc + 4) if pc + 4 < hi else 0
                if is_bad(w, nxt):
                    break
                if is_branch(w):
                    after = self.word(pc + 8) if pc + 8 < hi else 0
                    if pc + 4 >= hi or is_branch(nxt) or is_bad(nxt, after):
                        break
                    pcs += [pc, pc + 4]
                    break
                pcs.append(pc)
                pc += 4
                if pc in leaders:
                    break
                if len(pcs) >= MAX_BLOCK and pc < hi:
                    leaders.add(pc)
                    todo.insert(0, pc)
                    break
            if pcs:
                out.append(pcs)
        return out

    def emit(self, pcs):
        L = ['static uint32_t b_%08x(Ctx &c)' % pcs[0], '{', '    uint32_t *const r = c.r;']
        pend = None       # (reg, temp) scheduled by the previous instruction, not yet in its register
        lastload = None   # the last scheduled load so far
        branch = None     # index of the branch instruction once it has run
        if any(is_branch(self.word(p)) for p in pcs):
            L.append('    bool tk = false;')
            L.append('    uint32_t tg = 0u;')

        def leave(n, pc_expr, bp, bt, pending):
            if n == 0:
                return 'return 0u;'
            s = 'c.pc = %s; c.branchPending = %s; c.branchTarget = %s; c.pendingLoad = %s;' % (
                pc_expr, bp, bt, 'true' if pending else 'false')
            if lastload:
                s += ' c.anyLoad = true; c.loadReg = %du; c.loadValue = %s;' % lastload
            return s + ' return %du;' % n

        for k, pc in enumerate(pcs):
            w = self.word(pc)
            op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
            imm = w & 0xFFFF
            simm = imm - (0x10000 if imm & 0x8000 else 0)
            usimm = '0x%xu' % (simm & M32)
            in_slot = branch is not None
            # the interpreter state if we stop before this instruction
            if in_slot:
                before = leave(k, '0x%xu' % pc, 'tk', 'tk ? tg : 0u', pend)
            else:
                before = leave(k, '0x%xu' % pc, 'false', '0u', pend)
            body = []
            written = 0
            load = None
            mem = False

            def W(reg, expr):
                nonlocal written
                if reg:
                    body.append('r[%d] = %s;' % (reg, expr))
                    written = reg

            def LOAD(reg, expr):
                nonlocal load
                if reg:
                    body.append('const uint32_t t%d = %s;' % (k, expr))
                    load = (reg, 't%d' % k)
                else:
                    body.append('(void)(%s);' % expr)

            R = lambda n: 'r[%d]' % n
            S = lambda n: 'static_cast<int32_t>(r[%d])' % n
            if op == 0:
                if fn == 0x00:
                    W(rd, '%s << %d' % (R(rt), sa))
                elif fn == 0x02:
                    W(rd, '%s >> %d' % (R(rt), sa))
                elif fn == 0x03:
                    W(rd, 'static_cast<uint32_t>(%s >> %d)' % (S(rt), sa))
                elif fn == 0x04:
                    W(rd, '%s << (%s & 31u)' % (R(rt), R(rs)))
                elif fn == 0x06:
                    W(rd, '%s >> (%s & 31u)' % (R(rt), R(rs)))
                elif fn == 0x07:
                    W(rd, 'static_cast<uint32_t>(%s >> (%s & 31u))' % (S(rt), R(rs)))
                elif fn == 0x08:
                    body.append('tk = true; tg = %s;' % R(rs))
                elif fn == 0x09:
                    W(rd if rd else 31, '0x%xu' % (pc + 8))
                    body.append('tk = true; tg = %s;' % R(rs))
                elif fn == 0x10:
                    W(rd, '*c.hi')
                elif fn == 0x11:
                    body.append('*c.hi = %s;' % R(rs))
                elif fn == 0x12:
                    W(rd, '*c.lo')
                elif fn == 0x13:
                    body.append('*c.lo = %s;' % R(rs))
                elif fn == 0x18:
                    body.append('{ const int64_t m = static_cast<int64_t>(%s) * static_cast<int64_t>(%s); '
                                '*c.lo = static_cast<uint32_t>(m); *c.hi = static_cast<uint32_t>(static_cast<uint64_t>(m) >> 32u); }'
                                % (S(rs), S(rt)))
                elif fn == 0x19:
                    body.append('{ const uint64_t m = static_cast<uint64_t>(%s) * %s; '
                                '*c.lo = static_cast<uint32_t>(m); *c.hi = static_cast<uint32_t>(m >> 32u); }' % (R(rs), R(rt)))
                elif fn == 0x1A:
                    body.append('{ const int32_t a = %s, b = %s; '
                                'if (b == 0) { *c.lo = a >= 0 ? 0xFFFFFFFFu : 1u; *c.hi = static_cast<uint32_t>(a); } '
                                'else if (a == INT32_MIN && b == -1) { *c.lo = static_cast<uint32_t>(a); *c.hi = 0u; } '
                                'else { *c.lo = static_cast<uint32_t>(a / b); *c.hi = static_cast<uint32_t>(a %% b); } }'
                                % (S(rs), S(rt)))
                elif fn == 0x1B:
                    body.append('{ const uint32_t a = %s, b = %s; '
                                'if (b == 0u) { *c.lo = 0xFFFFFFFFu; *c.hi = a; } else { *c.lo = a / b; *c.hi = a %% b; } }'
                                % (R(rs), R(rt)))
                elif fn in (0x20, 0x22):
                    body.append('const int64_t s%d = static_cast<int64_t>(%s) %s static_cast<int64_t>(%s);'
                                % (k, S(rs), '+' if fn == 0x20 else '-', S(rt)))
                    body.append('if (s%d != static_cast<int32_t>(s%d)) { %s }' % (k, k, before))
                    W(rd, 'static_cast<uint32_t>(s%d)' % k)
                elif fn == 0x21:
                    W(rd, '%s + %s' % (R(rs), R(rt)))
                elif fn == 0x23:
                    W(rd, '%s - %s' % (R(rs), R(rt)))
                elif fn == 0x24:
                    W(rd, '%s & %s' % (R(rs), R(rt)))
                elif fn == 0x25:
                    W(rd, '%s | %s' % (R(rs), R(rt)))
                elif fn == 0x26:
                    W(rd, '%s ^ %s' % (R(rs), R(rt)))
                elif fn == 0x27:
                    W(rd, '~(%s | %s)' % (R(rs), R(rt)))
                elif fn == 0x2A:
                    W(rd, '%s < %s ? 1u : 0u' % (S(rs), S(rt)))
                elif fn == 0x2B:
                    W(rd, '%s < %s ? 1u : 0u' % (R(rs), R(rt)))
                else:
                    raise AssertionError(hex(w))
            elif op == 1:
                if rt & 0x10:
                    W(31, '0x%xu' % (pc + 8))
                body.append('tk = %s %s 0; tg = 0x%xu;' % (S(rs), '<' if (rt & 1) == 0 else '>=', (pc + 4 + (simm << 2)) & M32))
            elif op in (2, 3):
                if op == 3:
                    W(31, '0x%xu' % (pc + 8))
                body.append('tk = true; tg = 0x%xu;' % (((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)))
            elif op in (4, 5, 6, 7):
                cond = {4: '%s == %s' % (R(rs), R(rt)), 5: '%s != %s' % (R(rs), R(rt)),
                        6: '%s <= 0' % S(rs), 7: '%s > 0' % S(rs)}[op]
                body.append('tk = %s; tg = 0x%xu;' % (cond, (pc + 4 + (simm << 2)) & M32))
            elif op == 8:
                body.append('const int64_t s%d = static_cast<int64_t>(%s) + %d;' % (k, S(rs), simm))
                body.append('if (s%d != static_cast<int32_t>(s%d)) { %s }' % (k, k, before))
                W(rt, 'static_cast<uint32_t>(s%d)' % k)
            elif op == 9:
                W(rt, '%s + %s' % (R(rs), usimm))
            elif op == 0xA:
                W(rt, '%s < %d ? 1u : 0u' % (S(rs), simm))
            elif op == 0xB:
                W(rt, '%s < %s ? 1u : 0u' % (R(rs), usimm))
            elif op == 0xC:
                W(rt, '%s & 0x%xu' % (R(rs), imm))
            elif op == 0xD:
                W(rt, '%s | 0x%xu' % (R(rs), imm))
            elif op == 0xE:
                W(rt, '%s ^ 0x%xu' % (R(rs), imm))
            elif op == 0xF:
                W(rt, '0x%xu' % ((imm << 16) & M32))
            else:
                mem = True
                body.append('const uint32_t a%d = %s + %s;' % (k, R(rs), usimm))
                a = 'a%d' % k
                if op in (0x21, 0x25, 0x29):
                    body.append('if (%s & 1u) { %s }' % (a, before))
                elif op in (0x23, 0x2B):
                    body.append('if (%s & 3u) { %s }' % (a, before))
                if op == 0x20:
                    LOAD(rt, 'static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(rd8(c, %s))))' % a)
                elif op == 0x24:
                    LOAD(rt, 'rd8(c, %s)' % a)
                elif op == 0x21:
                    LOAD(rt, 'static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(rd16(c, %s))))' % a)
                elif op == 0x25:
                    LOAD(rt, 'rd16(c, %s)' % a)
                elif op == 0x23:
                    LOAD(rt, 'rd32(c, %s)' % a)
                elif op == 0x22:  # lwl: merges with the register as it is now (a load still in flight is not seen)
                    body.append('const uint32_t m%d = rd32(c, %s & ~3u);' % (k, a))
                    LOAD(rt, '(%s & (0x00FFFFFFu >> ((%s & 3u) * 8u))) | (m%d << (24u - (%s & 3u) * 8u))' % (R(rt), a, k, a))
                elif op == 0x26:  # lwr
                    body.append('const uint32_t m%d = rd32(c, %s & ~3u);' % (k, a))
                    LOAD(rt, '(%s & ~(0xFFFFFFFFu >> ((%s & 3u) * 8u))) | (m%d >> ((%s & 3u) * 8u))' % (R(rt), a, k, a))
                elif op == 0x28:
                    body.append('wr8(c, %s, %s);' % (a, R(rt)))
                elif op == 0x29:
                    body.append('wr16(c, %s, %s);' % (a, R(rt)))
                elif op == 0x2B:
                    body.append('wr32(c, %s, %s);' % (a, R(rt)))
                elif op == 0x2A:  # swl
                    body.append('{ const uint32_t o = rd32(c, %s & ~3u), v = %s, n = (%s & 3u) * 8u; '
                                'wr32(c, %s & ~3u, n == 24u ? v : (o & (0xFFFFFF00u << n)) | (v >> (24u - n))); }' % (a, R(rt), a, a))
                elif op == 0x2E:  # swr
                    body.append('{ const uint32_t o = rd32(c, %s & ~3u), v = %s, n = (%s & 3u) * 8u; '
                                'wr32(c, %s & ~3u, n == 0u ? v : (o & (0xFFFFFFFFu >> (32u - n))) | (v << n)); }' % (a, R(rt), a, a))
                else:
                    raise AssertionError(hex(w))

            L.append('    // %08x: %08x' % (pc, w))
            # memory helpers take the instruction's index: they bring the cycle counter up to date before a device access
            L += ['    ' + re.sub(r'\b((?:rd|wr)(?:8|16|32))\(c, ', r'\1(c, %du, ' % k, b) for b in body]
            # the load scheduled by the previous instruction lands now, unless this instruction wrote that register
            if pend and pend[0] != written:
                L.append('    r[%d] = %s;' % pend)
            pend = load
            if load:
                lastload = load
            if is_branch(w):
                branch = k
            elif mem:
                if op in (0x28, 0x29, 0x2A, 0x2B, 0x2E):
                    # a store into this block's own code: leave after it (the next entry compares the words again)
                    L.append('    if (((a%d & 0x1FFFFFFFu) + 3u - 0x%xu) < %du) c.io = true;' % (k, pcs[0], len(pcs) * 4 + 3))
                if in_slot:
                    L.append('    if (c.io) { %s }' % leave(k + 1, 'tk ? tg : 0x%xu' % (pc + 4), 'false', 'tk ? tg : 0u', pend))
                else:
                    L.append('    if (c.io) { %s }' % leave(k + 1, '0x%xu' % (pc + 4), 'false', '0u', pend))
        last = pcs[-1]
        if branch is not None:
            L.append('    ' + leave(len(pcs), 'tk ? tg : 0x%xu' % (last + 4), 'false', 'tk ? tg : 0u', pend))
        else:
            L.append('    ' + leave(len(pcs), '0x%xu' % (last + 4), 'false', '0u', pend))
        L.append('}')
        return L


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--state', required=True)
    ap.add_argument('--irx-dir', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--entry', required=True)
    ap.add_argument('--modules', default='')
    a = ap.parse_args()
    d = open(a.state, 'rb').read()
    d = d[max(d.find(IOPS_TAG), 0):]
    g = Gen(find_ram(d))
    want = {m.strip().upper() for m in a.modules.split(',') if m.strip()}
    files = {f.upper(): f for f in os.listdir(a.irx_dir)}
    out = ['// Generated by tools/iop_aotgen.py from %s. Do not edit: regenerate.' % os.path.basename(a.state),
           '#include "ps2x/iop/iop_aot.h"', '', '#include <cstdint>', '', 'namespace', '{',
           'using namespace ps2x::iop::aot;', '']
    table, words, total = [], [], 0
    for name, base in sorted(find_modules(d).items(), key=lambda kv: kv[1]):
        stem = name.rsplit('.', 1)[0]
        if name not in files or (want and stem not in want):
            continue
        text = irx_text(os.path.join(a.irx_dir, files[name]))
        if not text:
            continue
        lo, hi = base + text[0], base + text[0] + text[1]
        blocks = g.blocks(lo, hi)
        n = sum(len(b) for b in blocks)
        total += n
        print('%-14s 0x%06x-0x%06x  %5d blocks  %6d instructions' % (name, lo, hi, len(blocks), n))
        out.append('// %s 0x%x-0x%x' % (name, lo, hi))
        for pcs in blocks:
            out += g.emit(pcs)
            table.append('    {0x%xu, %du, kWords + %d, b_%08x},' % (pcs[0], len(pcs), len(words), pcs[0]))
            words += [g.word(p) for p in pcs]
    if not table:
        raise SystemExit('nothing to translate (modules not found in the state or in --irx-dir)')
    head = ['static const uint32_t kWords[] = {']
    for i in range(0, len(words), 8):
        head.append('    ' + ' '.join('0x%08xu,' % w for w in words[i:i + 8]))
    head += ['};', '']
    # kWords is used by the table only; the table goes last
    out = out[:9] + head + out[9:]
    out += ['', 'const Block kBlocks[] = {'] + table + ['};', '}', '',
            'void %s()' % a.entry, '{', '    registerBlocks(kBlocks, sizeof(kBlocks) / sizeof(kBlocks[0]));', '}', '']
    open(a.out, 'w', newline='\n').write('\n'.join(out))
    print('%d blocks, %d instructions -> %s' % (len(table), total, a.out))


if __name__ == '__main__':
    main()
