# Lists ZOE's actors per priority level from a save state (.p2s): the 9 lists FUN_00460350 runs every frame
# (GV actor system; map in docs/notes/02_zoe_internals.md "Frame loop"). Class names come from gcc 2 type info.
# usage: python zoe_actors.py <state file>
import collections, re, struct, sys

elf = open(r'A:/PS2Recomp/ZOE-Recompiled/game/SLUS_201.48', 'rb').read()
st = open(sys.argv[1], 'rb').read()
phoff = struct.unpack_from('<I', elf, 28)[0]
seg_off, seg_va = struct.unpack_from('<II', elf, phoff + 4)
sig = elf[seg_off + 0x200008 - seg_va:][:64]  # _start code: locates EE RAM inside the state file
off = st.find(sig)
assert off > 0, 'EE RAM not found'
base = off - 0x200008
r32 = lambda a: struct.unpack_from('<I', st, base + (a & 0x1ffffff))[0]
r16s = lambda a: struct.unpack_from('<h', st, base + (a & 0x1ffffff))[0]
r8 = lambda a: st[base + (a & 0x1ffffff)]

def cstr(a):
    b = st[base + (a & 0x1ffffff):][:48]
    return b.split(b'\0')[0].decode('latin1')

names = {}
def class_name(vt):
    # gcc 2 vtable: 8-byte entries {short delta, short index, pfn}; entry 1 is the type-info function, which loads the
    # mangled name ("6EV_JOB") with lui/addiu.
    if vt in names:
        return names[vt]
    fn, hi, found = r32(vt + 0xc), {}, '?'
    for i in range(24):
        ins = r32(fn + 4 * i)
        op, rs, rt, imm = ins >> 26, (ins >> 21) & 31, (ins >> 16) & 31, ins & 0xffff
        if op == 0x0f:
            hi[rt] = imm << 16
        elif op == 0x09 and rs in hi:
            s = cstr(hi[rs] + (imm - 0x10000 if imm & 0x8000 else imm))
            if re.fullmatch(r'\d+[A-Za-z_]\w*', s):
                found = re.sub(r'^\d+', '', s)
                break
    names[vt] = found
    return found

print('pause flags 0x4aa28c = %02x' % r8(0x4aa28c))
for lvl in range(9):
    mask, h, rows = r8(0x4aa290 + lvl * 0xc), r32(0x4aa294 + lvl * 0xc), collections.Counter()
    n = 0
    while h and n < 4000:
        vt = r32(h + 0xc)
        rows[(class_name(vt), '%08x' % r32(vt + 0x14), r8(h + 3))] += 1
        h, n = r32(h + 8), n + 1
    print('level %d (pause mask %02x): %d actors' % (lvl, mask, n))
    for (cls, act, state), c in rows.most_common(60):
        print('   %-24s act %s state %d  x%d' % (cls, act, state, c))
