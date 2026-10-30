#!/usr/bin/env python3
"""
sigmatch.py - name functions of a stripped PS2 (MIPS R5900) ELF by matching
relocation-masked byte signatures taken from a *reference* ELF that still has
its .symtab (same SDK / libc / libgcc generation), then verifying and
extending the result with the call graph.  Only needs Python 3 + numpy.

Pipeline
  1. Reference DB: every FUNC symbol of the reference ELF becomes a signature.
     Every 32-bit instruction word is kept except relocation-dependent fields,
     which are masked out:
       - j/jal 26-bit targets
       - lui immediates that are halves of main-RAM addresses (0x0001..0x01FF;
         literals such as MMIO 0x1000xxxx, scratchpad or float constants are kept)
       - the 16-bit immediates of instructions that consume such a lui result
         (addiu/ori/loads/stores...; tracked linearly per register, also through
         addu/or table indexing) and every 16-bit immediate whose base is $gp.
     Functions shorter than --min-insns, or whose masked signature is not
     unique inside the reference, are not used for plain matching.
  2. Exact scan: every 4-byte-aligned offset of the target's executable sections
     is compared with every signature (numpy prefix-hash index + full verify).
     Hits must be --min-align aligned (or be a jal target) and must not sit in a
     branch delay slot.  Kernel syscall stubs (li v1,N; syscall; jr ra; nop) are
     matched literally although they are shorter than --min-insns.
  3. Fuzzy scan: for reference functions without a match, 4-gram seeds vote for
     candidate addresses and positional / alignment-tolerant similarity is
     computed (SDK version drift); mutual-best assignment with a margin.
  4. Call graph (iterated to a fixed point):
       - for every matched pair the jal/j targets are paired instruction by
         instruction; callee names must agree (aliases allowed).  Agreement
         raises confidence, disagreement penalises or drops the weaker match.
       - shared-signature groups / multi-hit signatures are resolved by callee names
       - unnamed callees of trusted matches inherit the reference callee's name
         (source 'callgraph') if their bodies are structurally similar.
  5. Self-test (--self-test): reference scanned against itself with the target
     symbol table ignored; reports recovery at the right addresses and wrong hits.
  6. Optional anchors (--analyzer-toml): names of an independent tool, e.g. the
     ps2_analyzer SCE symbol scanner.  A first pass runs WITHOUT them (used for the
     agreement statistics), a second pass uses them as trusted anchors.
  7. Tags: functions referencing source-file strings ("pssman.cc") and gcc-2.95
     type_info getters (__tf<class>, __ti<class>, __rtti_*) -> extra CSVs.

Outputs (in --outdir, file prefix from --prefix): <p>_names.csv
(address,name,source,confidence), <p>_names_detail.csv, <p>_name_conflicts.csv,
<p>_name_ambiguous.csv, <p>_func_tags.csv, <p>_names_cxx_rtti.csv, report txt.

Typical use
  python sigmatch.py --ref <reference ELF with symbols> --target ZOE-Recompiled/game/SLUS_201.48 \
      --prefix zoe --outdir ZOE-Recompiled/work/names \
      --analyzer-toml ZOE-Recompiled/work/names/zoe_analyzer.toml
  python sigmatch.py --ref <reference ELF with symbols> --self-test --outdir <dir>
"""
import argparse
import collections
import csv
import difflib
import os
import re
import struct
import sys
import time

import numpy as np

M64 = (1 << 64) - 1
# lui immediates 0x0001..LUI_ADDR_MAX are treated as halves of main-RAM addresses
# (RAM is 32 MB = 0x0200_0000 -> hi16 <= 0x01FF); anything else is a literal.
LUI_ADDR_MAX = 0x01FF
HASH_MUL = 0x9E3779B97F4A7C15

# --------------------------------------------------------------------------
# ELF loading
# --------------------------------------------------------------------------


class Section:
    __slots__ = ("name", "type", "flags", "addr", "off", "size", "link", "info", "entsize")


class Sym:
    __slots__ = ("name", "addr", "size", "type", "bind", "shndx")


class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise ValueError("%s: not a 32-bit little-endian ELF" % path)
        (self.e_type, self.e_machine, _, self.entry, phoff, shoff, _, _,
         phentsize, phnum, shentsize, shnum, shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        self.segments = []
        for i in range(phnum):
            p_type, p_off, p_va, _, p_fsz, p_msz, p_flags, _ = struct.unpack_from(
                "<IIIIIIII", d, phoff + i * phentsize)
            if p_type == 1:
                self.segments.append((p_va, p_off, p_fsz, p_msz, p_flags))
        self.sections = []
        for i in range(shnum):
            v = struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
            s = Section()
            s.name, s.type, s.flags, s.addr, s.off, s.size, s.link, s.info, _, s.entsize = v
            self.sections.append(s)
        if shnum and shstrndx < shnum:
            so = self.sections[shstrndx].off
            for s in self.sections:
                e = d.index(b"\0", so + s.name)
                s.name = d[so + s.name:e].decode("latin1")
        self.symbols = []
        for s in self.sections:
            if s.type == 2:  # SHT_SYMTAB
                st = self.sections[s.link]
                for i in range(s.size // 16):
                    nm, val, size, info, _, shndx = struct.unpack_from("<IIIBBH", d, s.off + i * 16)
                    e = d.index(b"\0", st.off + nm)
                    y = Sym()
                    y.name = d[st.off + nm:e].decode("latin1")
                    y.addr, y.size, y.type, y.bind, y.shndx = val, size, info & 15, info >> 4, shndx
                    self.symbols.append(y)
        # executable code regions: (base, uint32 array)
        self.code = []
        for s in self.sections:
            if (s.flags & 4) and s.type == 1 and s.size >= 4 and s.addr:
                n = s.size // 4
                arr = np.frombuffer(d, dtype="<u4", count=n, offset=s.off)
                self.code.append((s.addr, arr, s.name))

    # -- memory helpers ----------------------------------------------------
    def file_offset(self, addr):
        for va, off, fsz, msz, _ in self.segments:
            if va <= addr < va + fsz:
                return off + (addr - va)
        return None

    def read(self, addr, n):
        o = self.file_offset(addr)
        if o is None:
            return None
        return self.data[o:o + n]

    def read32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b and len(b) == 4 else None

    def cstring(self, addr, maxlen=200):
        o = self.file_offset(addr)
        if o is None:
            return None
        e = self.data.find(b"\0", o, o + maxlen)
        if e < 0:
            return None
        return self.data[o:e]

    def word(self, addr):
        """instruction word at addr inside a code region, or None"""
        for base, arr, _ in self.code:
            if base <= addr < base + 4 * len(arr) and addr % 4 == 0:
                return int(arr[(addr - base) >> 2])
        return None

    def code_words(self, start, nwords):
        for base, arr, _ in self.code:
            if base <= start and start + 4 * nwords <= base + 4 * len(arr):
                i = (start - base) >> 2
                return arr[i:i + nwords]
        return None

    def in_code(self, addr):
        return any(base <= addr < base + 4 * len(arr) for base, arr, _ in self.code)


# --------------------------------------------------------------------------
# MIPS helpers
# --------------------------------------------------------------------------

# opcodes whose low 16 bits are an immediate/offset that may hold a %lo()/%gprel
_IMM_OPS = frozenset(
    [0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,          # addi..xori
     0x18, 0x19, 0x1A, 0x1B, 0x1E, 0x1F,                # daddi daddiu ldl ldr lq sq
     0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,    # loads
     0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,    # stores, cache
     0x31, 0x35, 0x36, 0x37, 0x39, 0x3D, 0x3E, 0x3F])   # fp/vu0 ld/st, ld, sd
_LOAD_LIKE_RT = frozenset([0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19, 0x1A, 0x1B, 0x1E,
                           0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37])
_R_ADD_LIKE = frozenset([0x20, 0x21, 0x2D, 0x25, 0x2C, 0x24])  # add addu daddu or dadd and


def dest_reg(w):
    """GPR written by instruction w, or None (conservative)."""
    o = w >> 26
    if o == 0:
        f = w & 63
        if f in (0x08, 0x0C, 0x0D, 0x0F, 0x11, 0x13, 0x1A, 0x1B, 0x29) or 0x30 <= f <= 0x36:
            # jr syscall break sync mthi mtlo div divu mtsa teq..
            return None
        return (w >> 11) & 31 or None      # incl. R5900 3-operand mult/multu (writes rd)
    if o == 3:
        return 31
    if o in _LOAD_LIKE_RT or o == 0x0F:
        return (w >> 16) & 31 or None
    if o in (0x10, 0x11, 0x12):
        # mfc0/mfc1/mfc2 (rs=0/0/1) and cfc1/cfc2 (rs=2) write rt
        rs_ = (w >> 21) & 31
        if (o == 0x12 and rs_ in (1, 2)) or (o != 0x12 and rs_ in (0, 2)):
            return (w >> 16) & 31 or None
        return None
    if o == 0x1C:  # MMI
        return (w >> 11) & 31 or None
    if o == 1:
        return 31 if ((w >> 16) & 31) in (0x10, 0x11) else None
    return None


def build_masks(words):
    """Per-word mask (1 = compare bit) for a function body (list/array of ints)."""
    n = len(words)
    masks = [0xFFFFFFFF] * n
    hi = [False] * 32
    for i in range(n):
        w = int(words[i])
        o = w >> 26
        if o == 2 or o == 3:
            masks[i] = 0xFC000000
        elif o == 0x0F:
            imm = w & 0xFFFF
            d = (w >> 16) & 31
            if 1 <= imm <= LUI_ADDR_MAX:
                # upper half of a main-RAM address -> relocation dependent
                masks[i] = 0xFFFF0000
                if d:
                    hi[d] = True
                continue
            # lui of a literal (MMIO 0x1000xxxx, scratchpad, float constants, 0): keep
            if d:
                hi[d] = False
            continue
        elif o in _IMM_OPS:
            r = (w >> 21) & 31
            if r == 28 or hi[r]:
                masks[i] = 0xFFFF0000
        # taint propagation
        if o == 0 and (w & 63) in _R_ADD_LIKE:
            d = (w >> 11) & 31
            if d and (hi[(w >> 21) & 31] or hi[(w >> 16) & 31]):
                hi[d] = True
                continue
        d = dest_reg(w)
        if d:
            hi[d] = False
    return masks


def jtarget(w, pc):
    return ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)


def is_jal(w):
    return (w >> 26) == 3


def is_j(w):
    return (w >> 26) == 2


JR_RA = 0x03E00008


# --------------------------------------------------------------------------
# .mdebug (ECOFF) source-file map: function address -> source file
# --------------------------------------------------------------------------


def load_mdebug(elf):
    """{function address: source file} from the MIPS .mdebug section, if present."""
    sec = next((s for s in elf.sections if s.name == ".mdebug" and s.size >= 96), None)
    if sec is None:
        return {}
    d = elf.data
    try:
        h = struct.unpack_from("<HHIIIIIIIIIIIIIIIIIIIIIII", d, sec.off)
        if h[0] != 0x7009:
            return {}
        cbSymOffset, cbSsOffset, ifdMax, cbFdOffset = h[10], h[16], h[19], h[20]
        out = {}
        for i in range(ifdMax):
            f = struct.unpack_from("<IIIIIIIIIIHHIIIIIII", d, cbFdOffset + i * 72)
            rss, issBase, isymBase, csym = f[1], f[2], f[4], f[5]
            so = cbSsOffset + issBase
            fname = d[so + rss:d.index(b"\0", so + rss)].decode("latin1")
            for j in range(csym):
                iss, val, bf = struct.unpack_from("<III", d, cbSymOffset + (isymBase + j) * 12)
                if (bf & 63) in (6, 14):       # stProc / stStaticProc
                    out[val] = fname
        return out
    except (struct.error, ValueError):
        return {}


def categorize(srcfile, name=""):
    if not srcfile:
        return "unknown"
    f = srcfile.replace("\\", "/")
    if "newlib/libm" in f:
        return "libm"
    if "newlib" in f:
        return "libc"
    if re.search(r"(^|/)(libgcc2\.c|fp-bit\.c|dp-bit\.c)$", f) or "/gcc/" in f:
        return "libgcc"
    if f.startswith("src/"):
        return "game"
    return "sdk"


# --------------------------------------------------------------------------
# Reference database
# --------------------------------------------------------------------------

_SKIP_SYM = re.compile(r"^(gcc2_compiled\.|__gnu_compiled_c|__gnu_compiled_cplusplus|zerobss|\$.*|\.L.*|L\d+)$")


class RefFunc:
    __slots__ = ("addr", "size", "name", "aliases", "nw", "words", "masks", "sig", "sigkey",
                 "bits", "unique", "calls", "id", "eligible", "why", "srcfile", "category")

    def __repr__(self):
        return "<Ref %s@%08X/%d>" % (self.name, self.addr, self.size)


def pick_primary(names):
    """primary name among aliases: prefer non-underscore, then shortest"""
    return sorted(names, key=lambda n: (n.startswith("_"), len(n), n))[0]


def build_refs(elf, min_insns=6, verbose=True):
    """Build RefFunc list from the FUNC symbols of `elf`.
    Returns (refs, names_at) where names_at maps address -> set of all code symbol names."""
    by_addr = collections.defaultdict(set)     # every code symbol name by address
    funcs = collections.defaultdict(list)
    for s in elf.symbols:
        if s.type == 2 and s.size >= 4 and s.name:
            funcs[(s.addr, s.size)].append(s.name)
        if s.type in (0, 2) and s.name and not _SKIP_SYM.match(s.name) and elf.in_code(s.addr):
            by_addr[s.addr].add(s.name)
    best = {}
    for (a, sz), names in funcs.items():
        if a not in best or sz > best[a][0]:
            best[a] = (sz, list(names))
        elif sz == best[a][0]:
            best[a][1].extend(names)
    mdebug = load_mdebug(elf)
    refs = []
    for a in sorted(best):
        sz, names = best[a]
        w = elf.code_words(a, sz // 4)
        if w is None:
            continue
        r = RefFunc()
        r.addr, r.size, r.nw = a, sz, sz // 4
        r.aliases = sorted(set(names) | by_addr.get(a, set()))
        r.name = pick_primary(names)
        r.words = w
        r.masks = np.array(build_masks(w.tolist()), dtype=np.uint32)
        r.sig = w & r.masks
        r.sigkey = (r.nw, r.sig.tobytes(), r.masks.tobytes())
        r.bits = int(sum(bin(int(m)).count("1") for m, x in zip(r.masks.tolist(), w.tolist()) if x != 0))
        r.calls = []
        for i, wi in enumerate(w.tolist()):
            o = wi >> 26
            if o == 3:
                r.calls.append((i, jtarget(wi, a + 4 * i)))
            elif o == 2:
                t = jtarget(wi, a + 4 * i)
                if not (a <= t < a + sz):
                    r.calls.append((i, t))
        r.srcfile = mdebug.get(a)
        r.category = categorize(r.srcfile, r.name)
        refs.append(r)
    for i, r in enumerate(refs):
        r.id = i
    cnt = collections.Counter(r.sigkey for r in refs)
    for r in refs:
        r.unique = cnt[r.sigkey] == 1
        r.eligible = r.nw >= min_insns and r.unique
        r.why = "" if r.eligible else ("short" if r.nw < min_insns else "duplicate-signature")
    if verbose:
        print("[ref] %s: %d functions, %d eligible (>= %d insns, unique masked signature)" %
              (elf.path, len(refs), sum(r.eligible for r in refs), min_insns))
    return refs, {a: set(v) for a, v in by_addr.items()}


# --------------------------------------------------------------------------
# Target helpers
# --------------------------------------------------------------------------


def jal_target_counts(elf):
    """Counter of jal targets (call sites) over all code regions."""
    cnt = collections.Counter()
    for base, arr, _ in elf.code:
        idx = np.nonzero((arr >> 26) == 3)[0]
        if len(idx) == 0:
            continue
        pcs = base + 4 * idx.astype(np.int64)
        t = ((pcs + 4) & 0xF0000000) | ((arr[idx].astype(np.int64) & 0x3FFFFFF) << 2)
        u, c = np.unique(t, return_counts=True)
        for a, n in zip(u.tolist(), c.tolist()):
            cnt[a] += n
    return cnt


def canon_window(elf, addr, n):
    """canonical (relocation-masked) words of the n instructions at addr (may be shorter at region end)"""
    for base, arr, _ in elf.code:
        if base <= addr < base + 4 * len(arr):
            i = (addr - base) >> 2
            zw = arr[i:i + n]
            return zw & np.array(build_masks(zw.tolist()), dtype=np.uint32)
    return None


def similarity(cr, cz):
    """similarity of canonical ref words `cr` against canonical target words `cz` (len(cz) >= len(cr) ideally).
    positional match ratio, falling back to an alignment-tolerant ratio."""
    n = len(cr)
    if n == 0 or cz is None or len(cz) == 0:
        return 0.0
    m = min(n, len(cz))
    pos = float(np.count_nonzero(cr[:m] == cz[:m])) / n
    if pos >= 0.97 or n > 500 or pos < 0.25:
        return pos
    sm_ = difflib.SequenceMatcher(None, cr.tolist(), cz.tolist(), autojunk=False)
    matched = sum(b.size for b in sm_.get_matching_blocks())
    return max(pos, matched / n)


def conf_from_bits(bits):
    """confidence of a unique signature match from its information content"""
    return max(0.30, min(0.97, 1.0 - 1.0 / (1.0 + bits / 100.0)))


class TargetIndex:
    """4-gram index of canonical words over the target's code (for fuzzy seeding)."""
    K = 4

    def __init__(self, elf):
        W, A = [], []
        for base, arr, _ in elf.code:
            W.append(np.array(arr))
            A.append(base + 4 * np.arange(len(arr), dtype=np.int64))
        self.W = np.concatenate(W)
        self.A = np.concatenate(A)
        gm = np.array(build_masks(self.W.tolist()), dtype=np.uint32)
        self.C = self.W & gm
        m = len(self.C) - self.K + 1
        h = np.zeros(m, dtype=np.uint64)
        mul = np.uint64(HASH_MUL)
        for j in range(self.K):
            h = h * mul + self.C[j:j + m].astype(np.uint64)
        self.order = np.argsort(h, kind="stable")
        self.hs = h[self.order]

    def lookup(self, hv):
        v = np.uint64(hv)
        lo = np.searchsorted(self.hs, v, "left")
        hi = np.searchsorted(self.hs, v, "right")
        return self.order[lo:hi]


class Match:
    __slots__ = ("addr", "ref", "name", "aliases", "method", "base", "conf", "sim", "agree", "disagree", "note")

    def __init__(self, addr, ref, name, method, base, sim=1.0, aliases=None, note=""):
        self.addr, self.ref, self.name, self.method = addr, ref, name, method
        self.base = self.conf = base
        self.sim = sim
        self.agree = self.disagree = 0
        self.aliases = set(aliases) if aliases else ({name} | (set(ref.aliases) if ref else set()))
        self.note = note

    @property
    def source(self):
        return {"exact": "signature", "fuzzy": "signature", "anchor": "sce_scanner",
                "callgraph": "callgraph", "callseq": "callgraph", "short_window": "short_window"}[self.method]


class Conflict:
    def __init__(self, addr, kind, name_a, src_a, name_b, src_b, detail=""):
        self.addr, self.kind, self.name_a, self.src_a, self.name_b, self.src_b, self.detail = \
            addr, kind, name_a, src_a, name_b, src_b, detail


# --------------------------------------------------------------------------
# Stage A: exact masked-signature scan
# --------------------------------------------------------------------------

K_PREFIX = 6

PRIORITY_RE = (r"^(sceMpeg(Delete|IsEnd|SetImageBuff|SetDecodeMode|IsRefBuffEmpty|Disp.*|.*PtsGap)"
               r"|sceDma.*|sceVif[01]Pk.*|sceSif.*IopHeap|sceSifGetDataTable|sceGsSetDefDBuff|sceGsPutDispEnv"
               r"|sceGsExecStoreImage|sceGsSetDefStoreImage)$")


def prefix_hash_py(vals):
    h = 0
    for v in vals:
        h = (h * HASH_MUL + int(v)) & M64
    return h


def scan_exact(elf, refs, min_insns):
    """All 4-byte-aligned offsets of every code region vs. every distinct masked signature of the
    reference functions with >= min_insns instructions.  Returns {sigkey: [addresses]}"""
    K = K_PREFIX
    reps = {}
    for r in refs:
        if r.nw >= min_insns and r.sigkey not in reps:
            reps[r.sigkey] = r
    groups = collections.defaultdict(list)
    for r in reps.values():
        groups[tuple(int(m) for m in r.masks[:K])].append(r)
    hits = collections.defaultdict(list)
    mul = np.uint64(HASH_MUL)
    for base, tw, _ in elf.code:
        n = len(tw)
        if n < K:
            continue
        m = n - K + 1
        for mk, rs_ in groups.items():
            h = np.zeros(m, dtype=np.uint64)
            for j in range(K):
                h = h * mul + (tw[j:j + m] & np.uint32(mk[j])).astype(np.uint64)
            table = collections.defaultdict(list)
            for r in rs_:
                table[prefix_hash_py(r.sig[:K].tolist())].append(r)
            keys = np.fromiter(table.keys(), dtype=np.uint64, count=len(table))
            for p in np.nonzero(np.isin(h, keys))[0].tolist():
                for r in table.get(int(h[p]), ()):
                    if p + r.nw <= n and np.array_equal(tw[p:p + r.nw] & r.masks, r.sig):
                        hits[r.sigkey].append(base + 4 * p)
    return hits


def is_branch_or_jump(w):
    o = w >> 26
    if o in (1, 2, 3, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17):
        return True
    if o == 0 and (w & 63) in (8, 9):
        return True
    if o in (0x10, 0x11, 0x12) and ((w >> 21) & 31) == 8:
        return True
    return False


def in_delay_slot(elf, a):
    p1 = elf.word(a - 4)
    return p1 is not None and is_branch_or_jump(p1)


def plausible_start(elf, a):
    """does a look like a function entry?  (preceded by padding nop, or by the delay slot of a jr / j)"""
    if in_delay_slot(elf, a):
        return False
    p1, p2 = elf.word(a - 4), elf.word(a - 8)
    if p1 is None or p1 == 0:
        return True      # first word of the code region / padding nop
    if p2 is not None and ((p2 >> 26) == 2 or ((p2 >> 26) == 0 and (p2 & 0x3F) == 0x08)):
        return True
    return False


class Group:
    """reference functions sharing one masked signature (members) and the target addresses matching it (hits)"""
    def __init__(self, members, hits):
        self.members, self.hits = members, hits
        self.n_members, self.n_hits = len(members), len(hits)


def stage_exact(elf, refs, called, min_align, min_insns, log):
    hits = scan_exact(elf, refs, min_insns)
    members = collections.defaultdict(list)
    for r in refs:
        if r.nw >= min_insns:
            members[r.sigkey].append(r)
    matches, groups, rejected = {}, [], []
    per_addr = collections.defaultdict(list)
    disamb = set()
    for sk, addrs in hits.items():
        mem = members[sk]
        good = [a for a in addrs if (a % min_align == 0 or called.get(a, 0) > 0) and not in_delay_slot(elf, a)]
        rejected += [(a, mem[0], "misaligned/delay-slot") for a in addrs if a not in good]
        if not good:
            continue
        if len(mem) == 1:
            r = mem[0]
            if len(good) > 1:
                # several places look like this function: prefer the ones that behave like function entries
                pl = [a for a in good if called.get(a, 0) > 0 and plausible_start(elf, a)]
                if len(pl) != 1:
                    pl = [a for a in good if called.get(a, 0) > 0]
                if len(pl) == 1:
                    good = pl
                    disamb.add((pl[0], r.id))
            if len(good) == 1:
                per_addr[good[0]].append(r)
                continue
        groups.append(Group(mem, good))
    for a, rs_ in per_addr.items():
        if len(rs_) > 1:
            rs_.sort(key=lambda r: -r.nw)
            if rs_[0].nw == rs_[1].nw:
                for r in rs_:
                    groups.append(Group([r], [a]))
                log.append(Conflict(a, "address_claimed_by_multiple_signatures", rs_[0].name, "signature",
                                    rs_[1].name, "signature",
                                    "same-length signatures %s" % ",".join(x.name for x in rs_)))
                continue
            log.append(Conflict(a, "prefix_signature_shadowed", rs_[0].name, "signature",
                                ",".join(x.name for x in rs_[1:]), "signature",
                                "kept the longest signature match"))
        r = rs_[0]
        c = conf_from_bits(r.bits)
        if called.get(a, 0) > 0:
            c = 1 - (1 - c) * 0.5
        if not plausible_start(elf, a) and called.get(a, 0) == 0:
            c *= 0.6        # neither called nor preceded by a function tail: suspicious
        if (a, r.id) in disamb:
            c *= 0.85
        matches[a] = Match(a, r, r.name, "exact", min(c, 0.985), 1.0,
                           note="disambiguated among several hits" if (a, r.id) in disamb else "")
    stage_syscall_stubs(elf, refs, matches, called, min_align, min_insns, log)
    return matches, groups, rejected


SYSCALL = 0x0000000C


def stage_syscall_stubs(elf, refs, matches, called, min_align, min_insns, log, max_words=8):
    """Kernel syscall stubs (addiu v1,zero,N ; syscall ; jr ra ; nop) are far shorter than --min-insns but the
    syscall number makes them unique: match them literally.  A stub key is only used if exactly one
    reference function has it and the target contains it exactly once."""
    keyed = collections.defaultdict(list)
    for r in refs:
        if r.nw < min_insns and r.nw <= max_words and not r.calls and any(int(x) == SYSCALL for x in r.words):
            keyed[tuple(int(x) for x in r.words)].append(r)
    n_new = 0
    for key, rs_ in keyed.items():
        if len(rs_) != 1:
            continue
        r = rs_[0]
        L = len(key)
        hits = []
        for base, tw, _ in elf.code:
            n = len(tw)
            cand = np.nonzero(tw[:n - L + 1] == np.uint32(key[0]))[0]
            for p in cand.tolist():
                if all(int(tw[p + j]) == key[j] for j in range(L)):
                    a = base + 4 * p
                    if (a % min_align == 0 or called.get(a, 0) > 0) and not in_delay_slot(elf, a):
                        hits.append(a)
        if len(hits) == 1 and hits[0] not in matches:
            matches[hits[0]] = Match(hits[0], r, r.name, "exact", 0.93, 1.0, note="kernel syscall stub (number match)")
            n_new += 1
    return n_new


def resolve_groups(elf, groups, matches, ref_names_at, called):
    """Pick, for reference functions whose masked signature is shared (or matched several places), the
    (reference, address) pairs whose call targets agree with the already named callees."""
    new = {}
    claimed = set(m.ref.id for m in matches.values() if m.ref is not None)
    for g in groups:
        for _round in range(4):
            hits = [z for z in g.hits if z not in matches and z not in new]
            mem = [r for r in g.members if r.id not in claimed]
            if not hits or not mem:
                break
            sup = {}
            for r in mem:
                for z in hits:
                    ag = dis = 0
                    for i, rt in r.calls:
                        zw = elf.word(z + 4 * i)
                        if zw is None or (zw >> 26) != (int(r.words[i]) >> 26):
                            continue
                        zm = matches.get(jtarget(zw, z + 4 * i)) or new.get(jtarget(zw, z + 4 * i))
                        rn = ref_names_at.get(rt)
                        if zm is None or not rn:
                            continue
                        if rn & zm.aliases:
                            ag += 1
                        else:
                            dis += 1
                    if ag >= 1 and dis == 0:
                        sup[(r.id, z)] = ag
            by_z, by_r = collections.defaultdict(list), collections.defaultdict(list)
            for rid, z in sup:
                by_z[z].append(rid)
                by_r[rid].append(z)
            progressed = False
            for rid, zs in by_r.items():
                if len(zs) == 1 and len(by_z[zs[0]]) == 1:
                    r = next(x for x in mem if x.id == rid)
                    z = zs[0]
                    c = 0.85 * conf_from_bits(r.bits)
                    new[z] = Match(z, r, r.name, "exact", min(c, 0.9), 1.0,
                                   note="shared/ambiguous signature resolved by callee names")
                    claimed.add(r.id)
                    progressed = True
            if not progressed:
                # elimination: every member is present in the target and only one is left
                if g.n_members == g.n_hits and len(mem) == 1 and len(hits) == 1 and g.n_members > 1:
                    r, z = mem[0], hits[0]
                    new[z] = Match(z, r, r.name, "exact", min(0.6 * conf_from_bits(r.bits), 0.6), 1.0,
                                   note="resolved by elimination within a shared-signature group")
                    claimed.add(r.id)
                break
    return new


# --------------------------------------------------------------------------
# Stage B: fuzzy signature match (SDK version drift)
# --------------------------------------------------------------------------


def stage_fuzzy(elf, refs, matches, tindex, called, min_insns, min_align, thr, log, used_refs):
    cands = {}
    mul_ = HASH_MUL
    for r in refs:
        if r.nw < min_insns or r.id in used_refs:
            continue
        cw = r.sig
        # choose informative 4-gram seeds
        seeds = []
        for i in range(r.nw - TargetIndex.K + 1):
            win = cw[i:i + TargetIndex.K]
            bits = sum(bin(int(m)).count("1") for m, x in zip(r.masks[i:i + 4].tolist(), r.words[i:i + 4].tolist()) if x != 0)
            if bits >= 96:
                seeds.append(i)
        if not seeds:
            continue
        if len(seeds) > 24:
            seeds = [seeds[int(k)] for k in np.linspace(0, len(seeds) - 1, 24)]
        votes = collections.Counter()
        for i in seeds:
            pos = tindex.lookup(prefix_hash_py(cw[i:i + 4].tolist()))
            if len(pos) > 300:
                continue
            for p in pos.tolist():
                votes[p - i] += 1
        need = 2 if len(seeds) >= 4 else 1
        top = [p for p, v in votes.most_common(40) if v >= need and p >= 0]
        res = []
        for p in top:
            a = int(tindex.A[p]) if p < len(tindex.A) else None
            if a is None or a in matches or in_delay_slot(elf, a):
                continue
            if a % min_align != 0 and called.get(a, 0) == 0:
                continue
            if called.get(a, 0) == 0 and not plausible_start(elf, a):
                continue
            cz = canon_window(elf, a, r.nw + 6)
            s = similarity(cr=cw, cz=cz)
            if s >= thr - 0.10:
                res.append((s, a))
        res.sort(reverse=True)
        if res:
            cands[r.id] = res
    # mutual-best assignment
    by_addr = collections.defaultdict(list)
    for rid, res in cands.items():
        for s, a in res:
            by_addr[a].append((s, rid))
    out = {}
    for rid, res in cands.items():
        s1, a1 = res[0]
        if s1 < thr:
            continue
        second_same_ref = res[1][0] if len(res) > 1 else 0.0
        others = sorted(by_addr[a1], reverse=True)
        if others[0][1] != rid:
            continue      # some other ref is a better fit for that address
        second_same_addr = others[1][0] if len(others) > 1 else 0.0
        margin = s1 - max(second_same_ref, second_same_addr)
        if margin < 0.005:
            if refs[rid].unique and s1 < 0.999:      # shared-signature twins are reported as ambiguous instead
                log.append(Conflict(a1, "fuzzy_tie", refs[rid].name, "signature", "", "signature",
                                    "sim %.3f, margin %.3f, competing candidates" % (s1, margin)))
            continue
        r = refs[rid]
        # count only informative words; tiny functions need near-perfect similarity
        if r.bits < 160 and s1 < 0.97:
            continue
        c = conf_from_bits(r.bits * s1 ** 4) * 0.92 * (0.75 + 0.25 * min(1.0, margin / 0.05))
        if called.get(a1, 0) > 0:
            c = 1 - (1 - c) * 0.6
        if a1 in out and out[a1].base >= c:
            continue
        out[a1] = Match(a1, r, r.name, "fuzzy", min(c, 0.95), s1, note="sim=%.3f" % s1)
    return out


# --------------------------------------------------------------------------
# Stage C: call-graph verification / propagation
# --------------------------------------------------------------------------


def collect_edges(elf, matches):
    """(caller_addr, callee_addr_in_target, ref_callee_addr) for every aligned call site of a trusted pair"""
    edges = []
    for a, m in matches.items():
        r = m.ref
        if r is None or m.sim < 0.80 or m.method == "callseq":
            continue
        for i, rt in r.calls:
            zw = elf.word(a + 4 * i)
            if zw is None or (zw >> 26) != (r.words[i] >> 26):
                continue
            edges.append((a, jtarget(zw, a + 4 * i), rt))
    return edges


def callgraph(elf, refs, ref_names_at, matches, groups, called, log, min_struct_sim=0.55, max_iter=12, verbose=True):
    ref_by_addr = {r.addr: r for r in refs}
    dropped = []
    it = 0
    added_total = 0
    while it < max_iter:
        it += 1
        gnew = resolve_groups(elf, groups, matches, ref_names_at, called)
        matches.update(gnew)
        added_total += len(gnew)
        edges = collect_edges(elf, matches)
        # ---- verification -------------------------------------------------
        while True:
            stats = collections.defaultdict(lambda: [0, 0])
            bad = []
            for ca, za, rt in edges:
                if ca not in matches:
                    continue
                zm = matches.get(za)
                rn = ref_names_at.get(rt)
                if zm is None or not rn:
                    continue
                if rn & zm.aliases:
                    stats[ca][0] += 1
                    stats[za][0] += 1
                else:
                    stats[ca][1] += 1
                    stats[za][1] += 1
                    bad.append((ca, za, rt))
            worst, wkey = None, None
            for a, (g, d) in stats.items():
                m = matches[a]
                if m.method == "anchor" or d <= g:
                    continue
                if m.base >= 0.85 and d < 3 * max(g, 1):
                    continue     # both sides look solid: treat as SDK version drift, penalise only
                key = (d - g, -m.base)
                if wkey is None or key > wkey:
                    worst, wkey = a, key
            if worst is None:
                break
            m = matches.pop(worst)
            dropped.append(m)
            g, d = stats[worst]
            other = [(ca, za) for ca, za, rt in bad if worst in (ca, za)]
            oth = matches.get(other[0][1] if other[0][0] == worst else other[0][0]) if other else None
            log.append(Conflict(worst, "callgraph_disagreement_dropped", m.name, m.source,
                                oth.name if oth else "", oth.source if oth else "",
                                "dropped %s match: %d disagreeing vs %d agreeing call edges" % (m.method, d, g)))
            edges = [e for e in edges if e[0] != worst]
        for a, m in matches.items():
            g, d = stats.get(a, (0, 0))
            m.agree, m.disagree = g, d
            if m.method in ("exact", "fuzzy", "anchor", "short_window"):
                c = 1 - (1 - m.base) * (0.5 ** g)
                c *= 0.7 ** d
                m.conf = min(0.995, c)
        for ca, za, rt in bad:
            if ca in matches and za in matches:
                log.append(Conflict(za, "callgraph_edge_mismatch", matches[za].name, matches[za].source,
                                    "/".join(sorted(ref_names_at.get(rt, ()))), "callee of %s" % matches[ca].name,
                                    "caller 0x%08X (%s) expects a different callee; both kept" % (ca, matches[ca].name)))
        # ---- propagation to unnamed callees -----------------------------
        votes = collections.defaultdict(lambda: collections.defaultdict(list))
        for ca, za, rt in edges:
            if za in matches or ca not in matches:
                continue
            rr = ref_by_addr.get(rt)
            if rr is None or not rr.name:
                continue
            votes[za][rr.id].append(matches[ca])
        new = {}
        for za, byref in votes.items():
            if za % 8 != 0 or not elf.in_code(za):
                continue
            scored = []
            for rid, callers in byref.items():
                rr = refs[rid]
                cz = canon_window(elf, za, rr.nw + 6)
                s = similarity(rr.sig, cz)
                need = 0.75 if rr.category == "game" else min_struct_sim
                if s < need:
                    continue
                uniq = {}
                for cm in callers:
                    uniq[cm.addr] = cm
                p = 1.0
                for cm in uniq.values():
                    p *= 1 - min(0.97, cm.conf) * 0.9
                sc = (1 - p) * (0.55 + 0.45 * s)
                scored.append((sc, s, rr, len(uniq)))
            if not scored:
                continue
            scored.sort(key=lambda x: -x[0])
            if len(scored) > 1 and scored[0][0] - scored[1][0] < 0.15:
                log.append(Conflict(za, "callgraph_vote_tie", scored[0][2].name, "callgraph", scored[1][2].name,
                                    "callgraph", "callers vote for different callees; left unnamed"))
                continue
            sc, s, rr, ncall = scored[0]
            if sc < 0.45:
                continue
            new[za] = Match(za, rr, rr.name, "callgraph", min(sc, 0.9), s,
                            note="callers=%d sim=%.2f" % (ncall, s))
        # a name given to two different addresses: keep the better one unless the names are static duplicates
        claimed = collections.defaultdict(list)
        for a, m in list(matches.items()) + list(new.items()):
            if m.ref is not None:
                claimed[m.ref.id].append(a)
        for rid, addrs in claimed.items():
            if len(addrs) > 1:
                for a in addrs:
                    if a in new:
                        log.append(Conflict(a, "callgraph_duplicate_name", new[a].name, "callgraph",
                                            "0x%08X" % [x for x in addrs if x != a][0], "already named",
                                            "reference function already matched elsewhere; not propagated"))
                        del new[a]
        matches.update(new)
        added_total += len(new)
        if verbose:
            print("[callgraph] iter %d: +%d propagated, +%d group-resolved, %d dropped so far" %
                  (it, len(new), len(gnew), len(dropped)))
        if not new and not gnew:
            break
    # final stats refresh
    edges = collect_edges(elf, matches)
    stats = collections.defaultdict(lambda: [0, 0])
    for ca, za, rt in edges:
        zm, rn = matches.get(za), ref_names_at.get(rt)
        if zm is None or not rn or ca not in matches:
            continue
        k = 0 if (rn & zm.aliases) else 1
        stats[ca][k] += 1
        stats[za][k] += 1
    for a, m in matches.items():
        g, d = stats.get(a, (0, 0))
        m.agree, m.disagree = g, d
        if m.method in ("exact", "fuzzy", "anchor", "short_window"):
            m.conf = min(0.995, (1 - (1 - m.base) * (0.5 ** g)) * (0.7 ** d))
        elif m.method == "callgraph":
            m.conf = min(0.95, (1 - (1 - m.base) * (0.6 ** g)) * (0.7 ** d))
    return dropped


# --------------------------------------------------------------------------
# Stage D: short functions inside the address window of their source file
# --------------------------------------------------------------------------


def masked_hits(elf, r, lo=0, hi=1 << 32):
    """addresses in [lo, hi) (all 4-byte-aligned offsets) whose words equal r's masked signature"""
    out = []
    for base, tw, _ in elf.code:
        s_, e_ = max(lo, base), min(hi, base + 4 * len(tw))
        p0 = (s_ - base + 3) >> 2
        p1 = ((e_ - base) >> 2) - r.nw + 1
        if p1 <= p0:
            continue
        ok = np.ones(p1 - p0, dtype=bool)
        for j in range(r.nw):
            ok &= (tw[p0 + j:p1 + j] & r.masks[j]) == r.sig[j]
        out += [base + 4 * (p0 + int(i)) for i in np.nonzero(ok)[0]]
    return out


def source_windows(matches, margin, min_conf=0.6):
    """{srcfile: (lo, hi)}: address span (+margin) of the target functions already matched from each
    reference source file"""
    win = {}
    for a, m in matches.items():
        r = m.ref
        if r is None or not r.srcfile or m.conf < min_conf or m.method == "short_window" or m.sim < 0.8:
            continue
        lo, hi = win.get(r.srcfile, (a, a + r.size))
        win[r.srcfile] = (min(lo, a), max(hi, a + r.size))
    return {f: (lo - margin, hi + margin) for f, (lo, hi) in win.items()}


def delta_context(matches):
    """{srcfile: sorted [(reference addr, target addr)]} of strong matches; used for the layout check:
    functions of one source file that were linked contiguously keep a constant (target - reference) offset."""
    ctx = collections.defaultdict(list)
    for a, m in matches.items():
        r = m.ref
        if r is None or not r.srcfile or m.conf < 0.6 or m.method == "short_window" or m.sim < 0.8:
            continue
        ctx[r.srcfile].append((r.addr, a))
    for v in ctx.values():
        v.sort()
    return ctx


def delta_predict(ctx, r):
    """target address predicted for reference r when the nearest matched functions of its source file on both
    sides share the same (target - reference) offset, else None"""
    import bisect
    lst = ctx.get(r.srcfile)
    if not lst:
        return None
    keys = [x[0] for x in lst]
    i = bisect.bisect_left(keys, r.addr)
    if i == 0 or i >= len(lst) or lst[i][0] == r.addr:
        return None
    d1 = lst[i - 1][1] - lst[i - 1][0]
    d2 = lst[i][1] - lst[i][0]
    return r.addr + d1 if d1 == d2 else None


def stage_short_window(elf, refs, matches, called, min_insns, min_align, margin, log, layout_apply=False):
    """Name short reference functions (< min_insns instructions) that plain signature matching skips:
    a short function is only searched inside the target address window spanned by the functions already
    matched from the same reference source file (+margin) and needs exactly one masked match there, at a
    plausible function entry outside every already matched function.  An address wanted by several
    references (identical short bodies) is left unnamed."""
    import bisect
    win = source_windows(matches, margin)
    claimed = set(m.ref.id for m in matches.values() if m.ref is not None)
    extents = sorted((a, a + m.ref.size) for a, m in matches.items() if m.ref is not None)
    ext_starts = [e[0] for e in extents]

    def inside_matched(a):
        i = bisect.bisect_right(ext_starts, a) - 1
        return i >= 0 and extents[i][0] < a < extents[i][1]

    dctx = delta_context(matches)
    cand, dconf = {}, set()
    for r in refs:
        if r.id in claimed or not (2 <= r.nw < min_insns) or not r.srcfile or r.srcfile not in win:
            continue
        lo, hi = win[r.srcfile]
        hits = [a for a in masked_hits(elf, r, lo, hi)
                if a not in matches and not inside_matched(a) and not in_delay_slot(elf, a)
                and (a % min_align == 0 or called.get(a, 0) > 0)
                and (called.get(a, 0) > 0 or plausible_start(elf, a))]
        pred = delta_predict(dctx, r)
        if len(hits) > 1 and layout_apply and pred is not None and pred in hits:
            twins_left = [x for x in refs if x.sigkey == r.sigkey and x.id not in claimed]
            if len(hits) >= len(twins_left):       # every identical-body twin is present in the target
                hits = [pred]          # several identical bodies in the window, layout offset picks one
        if len(hits) != 1:
            continue
        if pred is not None and pred != hits[0]:
            # both neighbours of r in its source file agree on where r must be, and this is not it
            log.append(Conflict(hits[0], "short_window_layout_contradiction", r.name, "short_window", "",
                                "layout", "unique window hit, but neighbouring same-file functions predict 0x%08X" % pred))
            continue
        cand[r.id] = hits[0]
        if pred is not None:
            dconf.add(r.id)
    want = collections.defaultdict(list)
    for rid, a in cand.items():
        want[a].append(rid)
    new = {}
    for a, rids in want.items():
        if len(rids) != 1:
            log.append(Conflict(a, "short_window_twins", refs[rids[0]].name, "short_window",
                                ",".join(refs[x].name for x in rids[1:]), "short_window",
                                "identical short bodies wanted the same address; left unnamed"))
            continue
        r = refs[rids[0]]
        c = 1 - (1 - conf_from_bits(r.bits)) * 0.5
        if called.get(a, 0) > 0:
            c = 1 - (1 - c) * 0.5
        lo, hi = win[r.srcfile]
        if r.id in dconf:
            c = 1 - (1 - c) * 0.5
        new[a] = Match(a, r, r.name, "short_window", min(c, 0.90 if r.id in dconf else 0.85), 1.0,
                       note="unique in window 0x%08X-0x%08X of %s%s" %
                            (lo, hi, os.path.basename(r.srcfile), "; layout offset confirmed" if r.id in dconf else ""))
    return new


# --------------------------------------------------------------------------
# Whole pipeline
# --------------------------------------------------------------------------


def run_pipeline(elf, refs, ref_names_at, anchors, args, verbose=True, tindex=None):
    """Returns dict with matches / ambiguous / conflicts / stats."""
    log = []
    t0 = time.time()
    called = jal_target_counts(elf)
    matches, groups, rejected = stage_exact(elf, refs, called, args.min_align, args.min_insns, log)
    n_exact = len(matches)
    if verbose:
        print("[exact] %d unique signature matches, %d shared/multi-hit groups, %d misaligned hits rejected (%.1fs)" %
              (n_exact, len(groups), len(rejected), time.time() - t0))
    # anchors (external names, e.g. SCE symbol scanner): reconcile with exact matches
    ref_by_name = collections.defaultdict(list)
    for r in refs:
        for n in r.aliases:
            ref_by_name[n].append(r)
    used_refs = set()
    for a, name in anchors.items():
        rs_ = ref_by_name.get(name, [])
        ref = rs_[0] if len(rs_) == 1 else None
        if a in matches:
            m = matches[a]
            if name not in m.aliases:
                log.append(Conflict(a, "anchor_vs_signature", name, "sce_scanner", m.name, "signature",
                                    "exact signature match from %s (0x%08X) disagrees with scanner; scanner kept" %
                                    (m.ref.name, m.ref.addr)))
            del matches[a]
        anchor_match = Match(a, ref, name, "anchor", 0.97, 1.0)
        if ref is not None:
            # SDK version drift: only trust call edges when the bodies still look alike
            cw = canon_window(elf, a, ref.nw + 6)
            anchor_match.sim = similarity(ref.sig, cw) if cw is not None else 0.0
            used_refs.add(ref.id)
        matches[a] = anchor_match
    for m in matches.values():
        if m.method == "exact":
            used_refs.add(m.ref.id)
    if not args.no_fuzzy:
        if tindex is None:
            tindex = TargetIndex(elf)
        fz = stage_fuzzy(elf, refs, matches, tindex, called, args.min_insns, args.min_align, args.fuzzy_thr, log, used_refs)
        for a, m in fz.items():
            if a not in matches:
                matches[a] = m
        if verbose:
            print("[fuzzy] %d additional near-signature matches (%.1fs)" % (len(fz), time.time() - t0))
    stage_counts = {"exact": n_exact, "fuzzy": sum(1 for m in matches.values() if m.method == "fuzzy")}
    dropped = []
    if not args.no_callgraph:
        dropped = callgraph(elf, refs, ref_names_at, matches, groups, called, log, verbose=verbose)
    if not args.no_short:
        for rnd in range(3):
            sw = stage_short_window(elf, refs, matches, called, args.min_insns, args.min_align, args.short_margin, log,
                                    layout_apply=args.short_layout_apply)
            if not sw:
                break
            matches.update(sw)
            if verbose:
                print("[short_window] round %d: +%d short functions named inside their source-file window" % (rnd + 1, len(sw)))
            if not args.no_callgraph:
                dropped += callgraph(elf, refs, ref_names_at, matches, groups, called, log, verbose=verbose)
    claimed = set(m.ref.id for m in matches.values() if m.ref is not None)
    ambiguous = {}
    for g in groups:
        left = [z for z in g.hits if z not in matches]
        for r in g.members:
            if r.id not in claimed and left:
                ambiguous[r.id] = left
    return {"matches": matches, "ambiguous": ambiguous, "rejected": rejected, "conflicts": log,
            "dropped": dropped, "called": called, "stage_counts": stage_counts, "tindex": tindex,
            "groups": groups}


# --------------------------------------------------------------------------
# External names (anchors) and output
# --------------------------------------------------------------------------


def load_anchors(path):
    """{address: name} from a ps2_analyzer TOML ("name@0xADDR" selectors) or an address,name CSV."""
    out = {}
    if path.lower().endswith(".toml"):
        txt = open(path, encoding="utf-8", errors="replace").read()
        for n, a in re.findall(r'"([^"@\s]+)@0x([0-9A-Fa-f]+)"', txt):
            out[int(a, 16)] = n
    else:
        with open(path, newline="", encoding="utf-8") as f:
            for row in csv.reader(f):
                if not row or not row[0].lower().startswith("0x"):
                    continue
                out[int(row[0], 16)] = row[1]
    return out


def hexa(a):
    return "0x%08X" % a


def write_csv(path, header, rows):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(header)
        w.writerows(rows)


def data_address_refs(elf):
    """Yield (instruction_address, materialised_address) for every lui + addiu/ori pair (tracked linearly
    per register, cleared on any write) and lui + load/store base use, over all code regions."""
    out = []
    for base, arr, _ in elf.code:
        val = {}
        for i, w in enumerate(arr.tolist()):
            o = w >> 26
            rs_, rt_ = (w >> 21) & 31, (w >> 16) & 31
            pc = base + 4 * i
            if o == 0x0F:
                imm = w & 0xFFFF
                if imm >= 0x10:
                    val[rt_] = imm << 16
                else:
                    val.pop(rt_, None)
                continue
            if o in (0x09, 0x0D) and rs_ in val:
                imm = w & 0xFFFF
                if o == 0x09 and imm & 0x8000:
                    imm -= 0x10000
                a = (val[rs_] + imm) & 0xFFFFFFFF
                out.append((pc, a))
                val.pop(rt_, None)
                continue
            if o in _IMM_OPS and o not in (0x09, 0x0D) and rs_ in val:
                imm = w & 0xFFFF
                if imm & 0x8000:
                    imm -= 0x10000
                out.append((pc, (val[rs_] + imm) & 0xFFFFFFFF))
            if o == 0 and (w & 63) in _R_ADD_LIKE:
                d = (w >> 11) & 31
                val.pop(d, None) if d else None
                continue
            d = dest_reg(w)
            if d:
                val.pop(d, None)
    return out


_SRC_RE = re.compile(rb"[\w./\-]*?([A-Za-z0-9_][A-Za-z0-9_\-]*\.(?:cc|cpp|c|h|S|s))(?![A-Za-z0-9_])")


def function_entry_estimate(elf, matches, called):
    """best-effort sorted list of function entry addresses: jal targets, matched functions and
    stack-frame prologues (addiu sp,sp,-N) that follow a function tail."""
    starts = set(a for a in called if elf.in_code(a) and a % 4 == 0)
    starts.update(matches.keys())
    starts.add(elf.entry)
    for base, arr, _ in elf.code:
        idx = np.nonzero(((arr >> 16) == 0x27BD) & ((arr & 0x8000) != 0))[0]
        for p in idx.tolist():
            a = base + 4 * p
            if a % 8 == 0 and plausible_start(elf, a):
                starts.add(a)
    return sorted(starts)




def rodata_strings(elf, pattern, min_len=4):
    """{address: (text, regex match)} for printable strings of non-exec allocated sections matching `pattern`"""
    out = {}
    for s in elf.sections:
        if s.type == 1 and (s.flags & 2) and not (s.flags & 4) and s.size:
            blob = elf.data[s.off:s.off + s.size]
            for m in re.finditer(rb"[ -~]{%d,200}" % min_len, blob):
                mm = pattern.search(m.group(0))
                if mm:
                    out[s.addr + m.start()] = (m.group(0).decode("latin1"), mm)
    return out


def rtti_pass(elf, starts, refs, matches):
    """gcc 2.95 type_info getters: small functions of the form
         if (node.vptr == 0) __rtti_user/__rtti_si(&node, "9EFF_FIELD" [, base]) ; return node;
    -> __tf<mangled> (function), __ti<mangled> (node object), __rtti_user / __rtti_si (helpers)."""
    import bisect
    names = {}
    for s in elf.sections:
        if s.type == 1 and (s.flags & 2) and not (s.flags & 4) and s.size:
            blob = elf.data[s.off:s.off + s.size]
            for m in re.finditer(rb"(?<![ -~])([0-9]{1,3})([A-Za-z_][A-Za-z0-9_]*)\x00", blob):
                if int(m.group(1)) == len(m.group(2)):
                    names[s.addr + m.start()] = m.group(0)[:-1].decode("latin1")
    if not names:
        return [], {}
    by_func = collections.defaultdict(list)
    for pc, a in refs:
        by_func[None].append((pc, a))
    per_func_strs = collections.defaultdict(list)
    per_func_refs = collections.defaultdict(list)
    for pc, a in refs:
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0:
            continue
        fs = starts[i]
        per_func_refs[fs].append((pc, a))
        if a in names:
            per_func_strs[fs].append((pc, a))
    rows = []
    helpers = collections.Counter()
    helper_kind = collections.defaultdict(collections.Counter)
    for fs, strs in per_func_strs.items():
        i = bisect.bisect_left(starts, fs)
        end = starts[i + 1] if i + 1 < len(starts) else fs + 4
        nw = (end - fs) // 4
        if len(strs) != 1 or nw > 48 or nw < 8:
            continue
        pc, sa = strs[0]
        words = [elf.word(fs + 4 * k) for k in range(nw)]
        jals = [(k, jtarget(w, fs + 4 * k)) for k, w in enumerate(words) if w is not None and (w >> 26) == 3]
        if not jals:
            continue
        # guard: a forward 'bne rX, zero' shortly before the string load, and the object test load before it
        k_pc = (pc - fs) // 4
        guard = any(w is not None and (w >> 26) == 5 and ((w >> 16) & 31) == 0 and (w & 0x8000) == 0
                    for w in words[max(0, k_pc - 10):k_pc])
        strict = guard and jals[-1][0] >= k_pc - 2      # string load may sit in the jal delay slot
        node = None
        for pc2, a2 in per_func_refs[fs]:
            if a2 not in names and pc2 < pc:
                node = a2
                break
        helper = jals[-1][1] if strict else None
        if helper is not None:
            helpers[helper] += 1
            # __rtti_user(node, name)  __rtti_si(node, name, base)  __rtti_class(node, name, bases, n)
            k_call = jals[-1][0]
            win = words[max(0, k_call - 8):k_call + 2]

            def sets_reg(r):
                return any(w is not None and (((w >> 26) in (0x0F, 9, 0x19, 0x0D) and ((w >> 16) & 31) == r) or
                                              ((w >> 26) == 0 and w != 0 and ((w >> 11) & 31) == r)) for w in win)
            kind = "class" if sets_reg(7) and sets_reg(6) else ("si" if sets_reg(6) else "user")
            helper_kind[helper][kind] += 1
        mangled = names[sa]
        m = matches.get(fs)
        rows.append((fs, "__tf" + mangled, 0.92 if strict else 0.70, "function", "" if m is None else m.name))
        if node is not None and strict:
            rows.append((node, "__ti" + mangled, 0.75, "data", ""))
    hrows = {}
    for h, n in helpers.items():
        if n >= 3:
            kind = helper_kind[h].most_common(1)[0][0]
            hrows[h] = ({"si": "__rtti_si", "class": "__rtti_class", "user": "__rtti_user"}[kind], 0.70, "function", n)
    return rows, hrows


def vtable_pass(elf, trows):
    """gcc 2.95 vtables: 8-byte entries {delta/index, pfn}; entry 0 holds the type_info getter __tf<class>,
    followed by the virtual functions.  Every word in a data section that equals the address of a __tf
    getter marks '__vt_<class>' at (word address - 4)."""
    tf_by_addr = {}
    for a, n, c, kind, cur in trows:
        if kind == "function" and n.startswith("__tf"):
            tf_by_addr[a] = n[4:]
    out = []
    for s in elf.sections:
        if s.type != 1 or not (s.flags & 2) or (s.flags & 4) or s.size < 8:
            continue
        arr = struct.unpack_from("<%dI" % (s.size // 4), elf.data, s.off)
        for i, v in enumerate(arr):
            if v not in tf_by_addr:
                continue
            a = s.addr + 4 * i
            if a % 8 != 4 or i == 0 or arr[i - 1] != 0:
                continue
            V = a - 4
            n_ent, k = 0, i - 1
            while k + 1 < len(arr):
                d, f = arr[k], arr[k + 1]
                if (d >> 16) != 0 and d != 0:
                    break
                if not (f == 0 or elf.in_code(f)):
                    break
                n_ent += 1
                k += 2
                if n_ent > 400:
                    break
            out.append((V, "__vt_" + tf_by_addr[v], 0.80, "vtable", "%d entries (slot 0 = __tf getter 0x%08X)" % (n_ent, v)))
    return out


def run_tags(elf, final, called, outdir, prefix, emit):
    import bisect
    starts = function_entry_estimate(elf, final["matches"], called)
    refs = data_address_refs(elf)
    # --- source-file strings -------------------------------------------------
    strings = {a: (t, m.group(1).decode("latin1")) for a, (t, m) in rodata_strings(elf, _SRC_RE).items()
               if len(m.group(1)) >= 4}
    rows = []
    seen = set()
    for pc, a in refs:
        if a in strings:
            i = bisect.bisect_right(starts, pc) - 1
            fs = starts[i] if i >= 0 else pc
            nxt = starts[i + 1] if i + 1 < len(starts) else fs
            key = (fs, strings[a][1], a)
            if key in seen:
                continue
            seen.add(key)
            m = final["matches"].get(fs)
            rows.append((hexa(fs), (nxt - fs) // 4, "source_file", strings[a][1], strings[a][0], hexa(pc), hexa(a),
                         m.name if m else "", m.source if m else ""))
    write_csv(os.path.join(outdir, "%s_func_tags.csv" % prefix),
              ["func_start_est", "func_words_est", "tag_kind", "tag", "string", "ref_insn", "string_addr",
               "current_name", "current_source"], sorted(rows))
    emit("source-file strings: %d strings, %d function tags (%d distinct functions)" %
         (len(strings), len(rows), len(set(r[0] for r in rows))))
    # --- gcc 2.95 RTTI getters -----------------------------------------------
    trows, hrows = rtti_pass(elf, starts, refs, final["matches"])
    out = []
    vrows = vtable_pass(elf, trows)
    for a, n, c, kind, cur in trows:
        out.append((hexa(a), n, "rtti_pattern", "%.2f" % c, kind, cur))
    for a, n, c, kind, note in vrows:
        out.append((hexa(a), n, "rtti_pattern", "%.2f" % c, kind, note))
    for h, (n, c, kind, cnt) in hrows.items():
        out.append((hexa(h), n, "rtti_pattern", "%.2f" % c, kind, "%d __tf callers" % cnt))
    write_csv(os.path.join(outdir, "%s_names_cxx_rtti.csv" % prefix),
              ["address", "name", "source", "confidence", "kind", "note"], sorted(set(out)))
    emit("gcc-2.95 RTTI: %d __tf getters, %d __ti nodes, %d __vt vtables, %d helpers %s" %
         (sum(1 for r in trows if r[3] == "function"), len(set(r[0] for r in trows if r[3] == "data")), len(vrows),
          len(hrows), sorted(n for n, c, k, x in hrows.values())))


# --------------------------------------------------------------------------
# Self test
# --------------------------------------------------------------------------


def self_test(elf, refs, names_at, args, outdir, prefix):
    print("\n=== SELF TEST: %s scanned against its own signatures (target symbol table ignored) ===" % elf.path)
    res = run_pipeline(elf, refs, names_at, {}, args, verbose=True)
    matches = res["matches"]
    lines = []

    def emit(s=""):
        print(s)
        lines.append(s)

    total = len(refs)
    elig = [r for r in refs if r.eligible]
    by_method = collections.defaultdict(lambda: [0, 0])
    wrong = []
    correct_ids = set()
    for a, m in sorted(matches.items()):
        ok = m.ref is not None and m.ref.addr == a
        by_method[m.method][0 if ok else 1] += 1
        if ok:
            correct_ids.add(m.ref.id)
        else:
            wrong.append((a, m))
    # raw false hits of the exact scan (before alignment / resolution)
    raw = scan_exact(elf, refs, args.min_insns)
    mem_by_key = collections.defaultdict(list)
    for r in refs:
        if r.nw >= args.min_insns:
            mem_by_key[r.sigkey].append(r)
    raw_false_refs = sum(1 for r in elig if any(x != r.addr for x in raw.get(r.sigkey, [])))
    raw_false_hits = sum(sum(1 for x in raw.get(r.sigkey, []) if x != r.addr) for r in elig)
    mis = sum(sum(1 for x in raw.get(r.sigkey, []) if x != r.addr and x % args.min_align != 0) for r in elig)
    emit("reference functions: %d  (eligible: %d, short: %d, duplicate-signature: %d)" %
         (total, len(elig), sum(r.why == "short" for r in refs), sum(r.why == "duplicate-signature" for r in refs)))
    emit("raw exact scan: %d of %d eligible signatures also match at other offsets (%d extra hits, %d of them misaligned)" %
         (raw_false_refs, len(elig), raw_false_hits, mis))
    emit("after alignment filter + resolution: ambiguous refs %d" % len(res["ambiguous"]))
    emit("")
    emit("matches by method (correct address / wrong):")
    for k in ("exact", "fuzzy", "callgraph", "short_window"):
        emit("  %-12s %5d / %d" % (k, by_method[k][0], by_method[k][1]))
    n_ok = len(correct_ids)
    emit("")
    emit("RECOVERY: %d / %d functions named at the right address = %.1f%%" % (n_ok, total, 100.0 * n_ok / total))
    n_ok_el = sum(1 for r in elig if r.id in correct_ids)
    emit("  eligible (>= %d insns, unique signature): %d / %d = %.1f%%" %
         (args.min_insns, n_ok_el, len(elig), 100.0 * n_ok_el / max(1, len(elig))))
    uniq_only = sum(1 for m in matches.values() if m.method == "exact" and m.ref.addr == m.addr and
                    not m.note.startswith(("shared", "resolved")) and m.note != "kernel syscall stub (number match)")
    ex_only = sum(1 for m in matches.values() if m.method == "exact" and m.ref.addr == m.addr)
    emit("  exact unique-signature matches: %d (%.1f%% of all functions, %.1f%% of eligible); "
         "+%d via shared-signature groups / syscall stubs" %
         (uniq_only, 100.0 * uniq_only / total, 100.0 * uniq_only / max(1, len(elig)), ex_only - uniq_only))
    emit("WRONG names: %d" % len(wrong))
    for a, m in wrong[:30]:
        emit("  0x%08X got %-28s (%s from ref 0x%08X)  true: %s" %
             (a, m.name, m.method, m.ref.addr if m.ref else 0, "/".join(sorted(names_at.get(a, []))) or "-"))
    # per category / reason
    emit("")
    emit("recovery by category:")
    cat = collections.defaultdict(lambda: [0, 0])
    for r in refs:
        cat[r.category][1] += 1
        if r.id in correct_ids:
            cat[r.category][0] += 1
    for k, (a, b) in sorted(cat.items()):
        emit("  %-8s %4d / %4d  %.1f%%" % (k, a, b, 100.0 * a / b))
    miss = [r for r in refs if r.id not in correct_ids]
    emit("")
    emit("not recovered: %d  (short %d, duplicate-signature %d, eligible-but-missed %d)" %
         (len(miss), sum(r.why == "short" for r in miss), sum(r.why == "duplicate-signature" for r in miss),
          sum(r.eligible for r in miss)))
    rows = []
    for r in miss:
        why = r.why or ("ambiguous-hits" if r.id in res["ambiguous"] else "other")
        rows.append((hexa(r.addr), r.name, r.nw, why, r.category, r.srcfile or ""))
    write_csv(os.path.join(outdir, "%s_selftest_unrecovered.csv" % prefix),
              ["address", "name", "nwords", "reason", "category", "srcfile"], rows)
    write_csv(os.path.join(outdir, "%s_selftest_wrong.csv" % prefix),
              ["address", "assigned_name", "method", "ref_addr", "true_names"],
              [(hexa(a), m.name, m.method, hexa(m.ref.addr) if m.ref else "", "/".join(sorted(names_at.get(a, []))))
               for a, m in wrong])
    with open(os.path.join(outdir, "%s_selftest_report.txt" % prefix), "w") as f:
        f.write("\n".join(lines) + "\n")
    return res


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def presence_report(elf, refs, final, called, args, outdir, prefix, emit):
    """For every reference function that is short (or matches --priority-regex) and did not get a name,
    say whether the target contains its masked bytes anywhere in .text."""
    pri = re.compile(args.priority_regex)
    matches = final["matches"]
    named = {}
    for a, m in matches.items():
        if m.ref is not None:
            named.setdefault(m.ref.id, []).append((a, m))
    twins = collections.defaultdict(list)
    for r in refs:
        twins[r.sigkey].append(r.name)
    win = source_windows(matches, args.short_margin)
    dctx = delta_context(matches)
    rows, prio = [], []
    stat = collections.Counter()
    for r in refs:
        is_short = 2 <= r.nw < args.min_insns
        is_pri = bool(pri.match(r.name))
        if not (is_short or is_pri):
            continue
        if r.id in named:
            a, m = named[r.id][0]
            status = "named"
            raw = plaus = []
            note = "%s 0x%08X (%s, conf %.2f)" % (m.name, a, m.method, m.conf)
        else:
            raw = masked_hits(elf, r)
            plaus = [a for a in raw if not in_delay_slot(elf, a) and (a % args.min_align == 0 or called.get(a, 0) > 0)
                     and (called.get(a, 0) > 0 or plausible_start(elf, a))]
            if not raw:
                status = "absent"
            elif not plaus:
                status = "absent_as_function"       # bytes occur only inside other code / not at an entry
            elif len(plaus) == 1:
                status = "present_unnamed_unique"
            else:
                status = "present_ambiguous"
            note = ""
        pred = ""
        if status != "named":
            pa = delta_predict(dctx, r)
            if pa is not None:
                w = elf.code_words(pa, r.nw)
                ok = w is not None and np.array_equal(w & r.masks, r.sig)
                pred = "0x%08X %s" % (pa, "bytes match (not applied)" if ok else "bytes differ")
                if ok:
                    stat["layout_prediction_verified"] += 1
        tw = [n for n in twins[r.sigkey] if n != r.name or twins[r.sigkey].count(n) > 1]
        win_s = ("0x%08X-0x%08X" % win[r.srcfile]) if r.srcfile in win else ""
        rows.append((r.name, hexa(r.addr), r.nw, r.srcfile or "", r.category, status, len(raw), len(plaus),
                     " ".join(hexa(x) for x in plaus[:8]) + (" ..." if len(plaus) > 8 else ""),
                     win_s, pred, ";".join(sorted(set(tw)))[:120], "priority" if is_pri else "", note))
        if is_short:
            stat[status] += 1
        if is_pri:
            prio.append((r, status, plaus, note, len(raw), pred))
    write_csv(os.path.join(outdir, "%s_short_presence.csv" % prefix),
              ["ref_name", "ref_addr", "nwords", "ref_srcfile", "category", "status", "raw_hits_in_text",
               "entry_like_hits", "entry_like_addresses", "source_window", "layout_prediction",
               "identical_body_refs", "priority", "note"],
              rows)
    emit("")
    emit("short reference functions (2..%d insns), status in the target: %s" % (args.min_insns - 1, dict(stat)))
    emit("priority list (%s):" % args.priority_regex)
    for r, status, plaus, note, nraw, pred in sorted(prio, key=lambda x: x[0].name):
        if status == "named":
            v = "FOUND, named: " + note
        elif status == "absent":
            v = "ABSENT (no masked match anywhere in .text)"
        elif status == "absent_as_function":
            v = "ABSENT as function (%d raw byte hits, none at a function entry)" % nraw
        elif status == "present_unnamed_unique":
            v = "FOUND but unnamed: 1 entry-like hit at 0x%08X (outside window or shared body)" % plaus[0]
        else:
            v = "FOUND but ambiguous: %d entry-like hits %s" % (len(plaus), " ".join(hexa(x) for x in plaus[:6]))
        emit("  %-26s nw=%-3d %s%s" % (r.name, r.nw, v, ("  [layout: %s]" % pred) if pred else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", required=True, help="reference ELF that still has a .symtab")
    ap.add_argument("--target", help="stripped ELF to name")
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--prefix", default=None, help="output file prefix (default: target file name)")
    ap.add_argument("--analyzer-toml", "--anchors", dest="anchors", default=None,
                    help="ps2_analyzer TOML (or address,name CSV) whose names are trusted anchors")
    ap.add_argument("--self-test", action="store_true", help="scan the reference ELF against itself, ignoring symbols")
    ap.add_argument("--ref-name", default=None, help="file prefix for self-test outputs (default: reference file name)")
    ap.add_argument("--min-insns", type=int, default=6)
    ap.add_argument("--min-align", type=int, default=8, help="function entry alignment; hits elsewhere are rejected")
    ap.add_argument("--fuzzy-thr", type=float, default=0.85)
    ap.add_argument("--min-conf", type=float, default=0.50, help="minimum confidence for the names CSV")
    ap.add_argument("--no-fuzzy", action="store_true")
    ap.add_argument("--no-short", action="store_true", help="skip the short-function window stage")
    ap.add_argument("--short-layout-apply", action=argparse.BooleanOptionalAction, default=True,
                    help="(default on) also name short functions with several identical bodies in their window "
                         "when the constant (target - reference) offset of the neighbouring same-file functions "
                         "picks one; --no-short-layout-apply disables")
    ap.add_argument("--short-margin", type=lambda x: int(x, 0), default=0x80,
                    help="bytes added on both sides of a source file's matched-address window (default 0x80)")
    ap.add_argument("--priority-regex", default=PRIORITY_RE,
                    help="reference names to report found/absent for in <prefix>_short_presence.csv")
    ap.add_argument("--no-callgraph", action="store_true")
    ap.add_argument("--no-tags", action="store_true", help="skip string/typeinfo scanning")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    ref = Elf(args.ref)
    if not ref.symbols:
        sys.exit("reference ELF has no .symtab")
    refs, names_at = build_refs(ref, args.min_insns)
    ref_prefix = os.path.splitext(os.path.basename(args.ref))[0]

    if args.self_test:
        self_test(ref, refs, names_at, args, args.outdir, args.ref_name or ref_prefix)
        if not args.target:
            return
    if not args.target:
        sys.exit("--target required (or --self-test)")

    target = Elf(args.target)
    prefix = args.prefix or os.path.splitext(os.path.basename(args.target))[0]
    anchors = load_anchors(args.anchors) if args.anchors else {}
    report = []

    def emit(s=""):
        print(s)
        report.append(s)

    emit("\n=== TARGET: %s ===" % args.target)
    emit("code regions: %s" % ", ".join("%s 0x%X-0x%X" % (n, b, b + 4 * len(a)) for b, a, n in target.code))
    if anchors:
        emit("anchors (%s): %d names" % (os.path.basename(args.anchors), len(anchors)))
        write_csv(os.path.join(args.outdir, "%s_analyzer_names.csv" % prefix), ["address", "name"],
                  [(hexa(a), n) for a, n in sorted(anchors.items())])

    # ---- pass 1: signature/callgraph without any external names (independent of the analyzer)
    pure = run_pipeline(target, refs, names_at, {}, args)
    tindex = pure["tindex"]
    # ---- pass 2: with anchors
    final = run_pipeline(target, refs, names_at, anchors, args, tindex=tindex) if anchors else pure

    # ---- analyzer agreement (pure pass) ------------------------------------
    conflicts = []
    agree_by = collections.defaultdict(lambda: [0, 0])
    if anchors:
        for a, n in sorted(anchors.items()):
            m = pure["matches"].get(a)
            if m is None:
                continue
            if n in m.aliases:
                agree_by[m.method][0] += 1
            else:
                agree_by[m.method][1] += 1
                conflicts.append(Conflict(a, "anchor_vs_%s" % m.source, n, "sce_scanner", m.name, m.source,
                                          "%s match from reference 0x%08X (%s), conf %.2f; scanner name kept" %
                                          (m.method, m.ref.addr if m.ref else 0, m.ref.name if m.ref else "", m.conf)))
    for c in final["conflicts"]:
        if c.kind == "anchor_vs_signature":
            continue      # already reported from the independent pass
        conflicts.append(c)

    # ---- same name at several addresses (only for names that are unique in the reference) ----
    ref_by_name = collections.defaultdict(list)
    for r in refs:
        for n in r.aliases:
            ref_by_name[n].append(r)
    addrs_by_name = collections.defaultdict(list)
    for a, m in final["matches"].items():
        addrs_by_name[m.name].append(a)
    conf_override = {}
    contradicted = set()
    for c in conflicts:
        if c.kind.startswith("anchor_vs_"):
            conf_override[c.addr] = 0.85      # scanner kept, but a second method disagrees
    for n, addrs in addrs_by_name.items():
        if len(addrs) < 2 or len(ref_by_name.get(n, [])) != 1:
            continue
        rf = ref_by_name[n][0]
        sims = {}
        for a in addrs:
            cw = canon_window(target, a, rf.nw + 6)
            sims[a] = similarity(rf.sig, cw) if cw is not None else 0.0
        best = max(addrs, key=lambda x: (sims[x], final["matches"][x].conf))
        for a in addrs:
            if a == best:
                continue
            m, mb = final["matches"][a], final["matches"][best]
            bad = sims[a] < 0.6 <= sims[best]
            if bad:
                conf_override[a] = 0.30
                contradicted.add(a)       # same name found elsewhere with a matching body: drop this row
            conflicts.append(Conflict(a, "same_name_multiple_addresses", n, m.source, n, mb.source,
                                      "reference %s (0x%08X, %d insns) is unique; body similarity %.2f here vs %.2f at 0x%08X (%s)%s" %
                                      (n, rf.addr, rf.nw, sims[a], sims[best], best, mb.source,
                                       "; row dropped from names.csv" if bad else "; both kept")))

    # ---- assemble names -----------------------------------------------------
    called = final["called"]
    rows, detail = {}, []
    for a, m in sorted(final["matches"].items()):
        conf = m.conf
        if m.method == "anchor":
            pm = pure["matches"].get(a)
            conf = 0.99 if (pm is not None and m.name in pm.aliases) else 0.97
        if a in conf_override:
            conf = conf_override[a]
        ok = (conf >= args.min_conf or m.method == "anchor") and a not in contradicted
        if ok:
            rows[a] = (m.name, m.source, conf)
        r = m.ref
        detail.append((hexa(a), m.name, m.source, "%.2f" % conf, m.method, hexa(r.addr) if r else "",
                       r.name if r else "", r.srcfile or "" if r else "", r.category if r else "",
                       r.nw if r else "", "%.3f" % m.sim, m.agree, m.disagree, called.get(a, 0),
                       "/".join(sorted(m.aliases)), m.note, 1 if ok else 0))
    write_csv(os.path.join(args.outdir, "%s_names.csv" % prefix), ["address", "name", "source", "confidence"],
              [(hexa(a), n, s, "%.2f" % c) for a, (n, s, c) in sorted(rows.items())])
    write_csv(os.path.join(args.outdir, "%s_names_detail.csv" % prefix),
              ["address", "name", "source", "confidence", "method", "ref_addr", "ref_name", "ref_srcfile", "category",
               "nwords", "sim", "edges_agree", "edges_disagree", "jal_callers", "aliases", "note", "in_names_csv"], detail)
    write_csv(os.path.join(args.outdir, "%s_name_conflicts.csv" % prefix),
              ["address", "kind", "name_a", "source_a", "name_b", "source_b", "detail"],
              sorted(set((hexa(c.addr), c.kind, c.name_a, c.src_a, c.name_b, c.src_b, c.detail) for c in conflicts),
                     key=lambda r: (r[0], r[1], r[2])))
    amb_rows = []
    for rid, addrs in sorted(final["ambiguous"].items()):
        r = refs[rid]
        amb_rows.append((r.name, hexa(r.addr), r.nw, len(addrs), " ".join(hexa(x) for x in addrs[:12]) +
                         (" ..." if len(addrs) > 12 else "")))
    write_csv(os.path.join(args.outdir, "%s_name_ambiguous.csv" % prefix),
              ["ref_name", "ref_addr", "nwords", "n_hits", "hits"], amb_rows)

    # ---- optional tags -------------------------------------------------------
    if not args.no_tags:
        run_tags(target, final, called, args.outdir, prefix, emit)

    presence_report(target, refs, final, called, args, args.outdir, prefix, emit)

    # ---- report ---------------------------------------------------------------
    src_cnt = collections.Counter(s for n, s, c in rows.values())
    emit("")
    emit("names written: %d (min confidence %.2f)" % (len(rows), args.min_conf))
    for k in ("sce_scanner", "signature", "callgraph", "short_window"):
        emit("  %-12s %d" % (k, src_cnt.get(k, 0)))
    mc = collections.Counter(m.method for m in final["matches"].values())
    emit("  methods: %s" % dict(mc))
    if anchors:
        tot = sum(a + b for a, b in agree_by.values())
        ag = sum(a for a, b in agree_by.values())
        emit("")
        emit("independent pass (no scanner names used) vs analyzer's %d names:" % len(anchors))
        emit("  covered by sigmatch: %d (%.1f%%)" % (tot, 100.0 * tot / len(anchors)))
        emit("  agree: %d, disagree: %d  -> agreement %.2f%%" % (ag, tot - ag, 100.0 * ag / max(1, tot)))
        for k, (a, b) in sorted(agree_by.items()):
            emit("    %-10s agree %d, disagree %d" % (k, a, b))
        silent = [n for a, n in anchors.items() if a not in pure["matches"]]
        ref_names = set(n for r in refs for n in r.aliases)
        emit("  analyzer-only (sigmatch silent): %d, of which the reference ELF has no function of that name: %d" %
             (len(silent), sum(1 for n in silent if n not in ref_names)))
        new_sig = [a for a, m in pure["matches"].items() if a not in anchors]
        emit("  sigmatch-only addresses in the independent pass: %d" % len(new_sig))
        emit("  analyzer names: %d, with 'sce' prefix %d, present in the reference ELF by name %d" %
             (len(anchors), sum(1 for n in anchors.values() if n.lower().startswith(("sce", "_sce"))),
              sum(1 for n in anchors.values() if n in ref_names)))
    kc = collections.Counter(c.kind for c in conflicts)
    emit("conflict rows: %d  %s" % (len(conflicts), dict(kc)))
    emit("ambiguous references (matched several addresses): %d" % len(amb_rows))
    with open(os.path.join(args.outdir, "sigmatch_report_%s.txt" % prefix), "w") as f:
        f.write("\n".join(report) + "\n")


if __name__ == "__main__":
    main()
