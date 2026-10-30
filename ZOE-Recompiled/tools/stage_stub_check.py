#!/usr/bin/env python3
"""Validate the Zone of the Enders stage stubs (<stage>.bin) against the interpreter whitelist.

src/zoe_overrides.cpp executes each stub natively from guest RAM (the stub sits at a variable heap address, so it
cannot be recompiled). It only accepts stubs made of the instruction forms below. This script is a Python model of
that SAME whitelist/grammar, run over every .bin:

  * every bin must be fully accepted (no other opcode, register, jump target or immediate form);
  * the number of registrations per bin must match work/stages/stage_bins.csv (3,295 in total);
  * the (call offset, hash, function) triples produced by modelling a0/a1 must match
    work/stages/stage_bin_registrations.csv;
  * the k* constants in src/zoe_overrides.cpp must equal the constants below (parsed from the C++ source), so the two
    whitelists cannot drift apart silently.

Grammar (identical in scanStub() in zoe_overrides.cpp):
  word 0..3 : addiu sp,sp,-0x10 ; sd ra,0(sp) ; jal 0x43C8E8 ; nop
  then      : { nop | lui a0/a1,imm | addiu a1,a1,imm | ori a0,a0,imm
                | jal 0x43BB90 + delay slot (nop or a0/a1 load) | ld ra,0(sp) }
  end       : (j 0x43BB90 | jr ra) + delay slot `addiu sp,sp,0x10`
  rules     : ld ra only once and after sd ra; no jal 0x43BB90 after ld ra; the terminator needs ld ra done;
              addiu sp/sd ra/jal 0x43C8E8 only at their prologue positions (sp restore only in the terminator's slot).
  `jr ra` as terminator is allowed by the interpreter but does not occur in the 22 bins.

usage: stage_stub_check.py [--bins DIR] [--csv stage_bins.csv] [--regs stage_bin_registrations.csv]
                           [--cpp zoe_overrides.cpp] [--no-cpp-check] [--self-test] [-v]
Exit status 0 = everything accepted and consistent, 1 otherwise.
"""
import argparse
import csv
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # ZOE-Recompiled
STAGES = os.path.join(ROOT, "work", "stages")

EXPECTED_TOTAL = 3295
BASE = 0x00900020  # any 16-byte aligned address: the stubs are position independent (model only)

# ---- Whitelist. Names/values must equal the k* constants in src/zoe_overrides.cpp (checked by --cpp). ----
CONSTANTS = {
    "kStubPtrAddr": 0x0049DE50,
    "kInitFn": 0x0043C8E8,
    "kRegisterFn": 0x0043BB90,
    "kMaxStubWords": 0x1000,
    "kWordNop": 0x00000000,
    "kWordAddiuSpDown": 0x27BDFFF0,
    "kWordAddiuSpUp": 0x27BD0010,
    "kWordSdRa": 0xFFBF0000,
    "kWordLdRa": 0xDFBF0000,
    "kWordJalInit": 0x0C10F23A,
    "kWordJalRegister": 0x0C10EEE4,
    "kWordJRegister": 0x0810EEE4,
    "kWordJrRa": 0x03E00008,
    "kHiLuiA0": 0x3C04,
    "kHiLuiA1": 0x3C05,
    "kHiAddiuA1": 0x24A5,
    "kHiOriA0": 0x3484,
}
C = CONSTANTS
assert C["kWordJalInit"] == 0x0C000000 | (C["kInitFn"] >> 2)
assert C["kWordJalRegister"] == 0x0C000000 | (C["kRegisterFn"] >> 2)
assert C["kWordJRegister"] == 0x08000000 | (C["kRegisterFn"] >> 2)

FULL_WORDS = {
    C["kWordNop"]: "nop",
    C["kWordAddiuSpDown"]: "addiu_sp_down",
    C["kWordAddiuSpUp"]: "addiu_sp_up",
    C["kWordSdRa"]: "sd_ra",
    C["kWordLdRa"]: "ld_ra",
    C["kWordJalInit"]: "jal_init",
    C["kWordJalRegister"]: "jal_register",
    C["kWordJRegister"]: "j_register",
    C["kWordJrRa"]: "jr_ra",
}
HI_WORDS = {
    C["kHiLuiA0"]: "lui_a0",
    C["kHiLuiA1"]: "lui_a1",
    C["kHiAddiuA1"]: "addiu_a1",
    C["kHiOriA0"]: "ori_a0",
}
ARG_OPS = {"lui_a0", "lui_a1", "addiu_a1", "ori_a0"}


def decode(w):
    op = FULL_WORDS.get(w)
    if op is not None:
        return op
    return HI_WORDS.get(w >> 16, "bad")


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


class Scan:
    def __init__(self):
        self.ok = False
        self.callbacks = 0
        self.end_pc = 0          # offset past the terminator's delay slot
        self.bad_pc = 0
        self.bad_word = 0
        self.why = None
        self.ops = []            # [(offset, op)] of everything accepted (delay slots included)
        self.calls = []          # [(call offset, a0, a1)] for jal/j 0x43BB90, from modelling a0/a1
        self.resume = []         # return addresses of the calls inside the stub (offsets)


def scan_stub(data, base=BASE):
    """Model of scanStub() in zoe_overrides.cpp. data = raw bytes at `base`; returns a Scan."""
    s = Scan()

    def read(off):
        if off < 0 or off % 4 or off + 4 > len(data):
            return None
        return struct.unpack_from("<I", data, off)[0]

    def fail(at, word, why):
        s.bad_pc, s.bad_word, s.why = base + at, word, why
        return s

    a0 = a1 = 0

    def apply_arg(op, w):
        nonlocal a0, a1
        imm = w & 0xFFFF
        if op == "lui_a0":
            a0 = (imm << 16) & 0xFFFFFFFF
        elif op == "lui_a1":
            a1 = (imm << 16) & 0xFFFFFFFF
        elif op == "addiu_a1":
            a1 = (a1 + sext16(imm)) & 0xFFFFFFFF
        elif op == "ori_a0":
            a0 = a0 | imm

    ra_restored = False
    pc = 0
    n = 0
    while n < C["kMaxStubWords"]:
        w = read(pc)
        if w is None:
            return fail(pc, 0, "outside guest RAM or unaligned")
        op = decode(w)
        if n == 0 and op != "addiu_sp_down":
            return fail(pc, w, "prologue word 0 is not addiu sp,sp,-0x10")
        if n == 1 and op != "sd_ra":
            return fail(pc, w, "prologue word 1 is not sd ra,0(sp)")
        if n == 2 and op != "jal_init":
            return fail(pc, w, "prologue word 2 is not jal 0x43C8E8")

        if op in ("nop", "lui_a0", "lui_a1", "addiu_a1", "ori_a0"):
            apply_arg(op, w)
            s.ops.append((pc, op))
        elif op == "addiu_sp_down":
            if n != 0:
                return fail(pc, w, "addiu sp,sp,-0x10 outside the prologue")
            s.ops.append((pc, op))
        elif op == "addiu_sp_up":
            return fail(pc, w, "addiu sp,sp,0x10 outside a terminator delay slot")
        elif op == "sd_ra":
            if n != 1:
                return fail(pc, w, "sd ra outside the prologue")
            s.ops.append((pc, op))
        elif op == "ld_ra":
            if n < 4 or ra_restored:
                return fail(pc, w, "unexpected ld ra,0(sp)")
            ra_restored = True
            s.ops.append((pc, op))
        elif op in ("jal_init", "jal_register"):
            if (n != 2) if op == "jal_init" else (n < 4 or ra_restored):
                return fail(pc, w, "unexpected call")
            dw = read(pc + 4)
            if dw is None:
                return fail(pc + 4, 0, "delay slot outside guest RAM")
            dop = decode(dw)
            if (dop != "nop") if op == "jal_init" else (dop != "nop" and dop not in ARG_OPS):
                return fail(pc + 4, dw, "delay slot of a call is not a whitelisted a0/a1 load or nop")
            apply_arg(dop, dw)  # the delay slot executes before the callee runs
            s.ops.append((pc, op))
            s.ops.append((pc + 4, dop))
            if op == "jal_register":
                s.callbacks += 1
                s.calls.append((pc, a0, a1))
            s.resume.append(pc + 8)
            n += 1
            pc += 4
        elif op in ("j_register", "jr_ra"):
            if n < 4 or not ra_restored:
                return fail(pc, w, "terminator before ld ra,0(sp)")
            dw = read(pc + 4)
            if dw is None:
                return fail(pc + 4, 0, "delay slot outside guest RAM")
            if dw != C["kWordAddiuSpUp"]:
                return fail(pc + 4, dw, "terminator delay slot is not addiu sp,sp,0x10")
            s.ops.append((pc, op))
            s.ops.append((pc + 4, "addiu_sp_up"))
            if op == "j_register":
                s.callbacks += 1
                s.calls.append((pc, a0, a1))
            s.end_pc = pc + 8
            s.ok = True
            return s
        else:
            return fail(pc, w, "instruction not in the stage-stub whitelist")
        n += 1
        pc += 4
    return fail(pc, 0, "no terminator within the scan limit")


def accept_at(data, address, base=BASE):
    """Model of acceptStubAt(): address (absolute) is the stub entry or the return address of one of its calls."""
    s = scan_stub(data, base)
    if not s.ok:
        return False
    off = address - base
    if off == 0:
        return True
    return off >= 16 and off < s.end_pc and off % 4 == 0 and (off - 8) in [r - 8 for r in s.resume]


# ------------------------------------------------------------------------------------------------------------
def load_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def check_cpp(path):
    """Compare the k* constants of the C++ interpreter with CONSTANTS. Returns a list of problems."""
    if not os.path.isfile(path):
        return ["C++ source not found: %s" % path]
    text = open(path, encoding="utf-8").read()
    found = {}
    for m in re.finditer(r"constexpr\s+uint32_t\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)u\s*;", text):
        found[m.group(1)] = int(m.group(2), 16)
    problems = []
    for name, want in CONSTANTS.items():
        if name not in found:
            problems.append("constant %s missing in %s" % (name, os.path.basename(path)))
        elif found[name] != want:
            problems.append("constant %s differs: C++ 0x%X, Python 0x%X" % (name, found[name], want))
    for name in found:
        if name not in CONSTANTS:
            problems.append("C++ constant %s has no Python counterpart" % name)
    return problems


def load_bins(bins_dir):
    out = {}
    for fn in sorted(os.listdir(bins_dir)):
        if fn.endswith(".bin"):
            with open(os.path.join(bins_dir, fn), "rb") as f:
                out[fn] = f.read()
    return out


def run_check(args):
    problems = []
    bins = load_bins(args.bins)
    table = {r["name"]: r for r in load_csv(args.csv)}
    regs = {}
    if os.path.isfile(args.regs):
        for r in load_csv(args.regs):
            regs.setdefault(r["bin"], []).append((int(r["offset"], 16), int(r["hash"], 16), int(r["fnptr"], 16)))
    else:
        print("note: %s not found, skipping the (hash, function) cross-check" % args.regs)

    if sorted(bins) != sorted(table):
        problems.append("bin set differs from stage_bins.csv: only on disk %s, only in csv %s"
                        % (sorted(set(bins) - set(table)), sorted(set(table) - set(bins))))

    total = 0
    csv_total = 0
    print("%-12s %6s %6s %9s %8s  %s" % ("bin", "words", "regs", "code_end", "resume", "status"))
    for name, data in bins.items():
        s = scan_stub(data)
        row = table.get(name)
        status = "ok"
        if not s.ok:
            status = "REJECTED: %s (word 0x%08X at +0x%X)" % (s.why, s.bad_word, s.bad_pc - BASE)
            problems.append("%s: %s" % (name, status))
        else:
            total += s.callbacks
            if row is not None:
                csv_total += int(row["registrations"])
                if s.callbacks != int(row["registrations"]):
                    problems.append("%s: %d registrations, stage_bins.csv says %s" % (name, s.callbacks, row["registrations"]))
                    status = "COUNT MISMATCH"
                if s.end_pc != int(row["code_end"], 16):
                    problems.append("%s: code end 0x%X, stage_bins.csv says %s" % (name, s.end_pc, row["code_end"]))
                    status = "CODE_END MISMATCH"
                if len(data) != int(row["size"]):
                    problems.append("%s: file size %d, stage_bins.csv says %s" % (name, len(data), row["size"]))
            if name in regs:
                got = [(pc, a0, a1) for (pc, a0, a1) in s.calls]
                if got != regs[name]:
                    diff = [i for i, (x, y) in enumerate(zip(got, regs[name])) if x != y][:3]
                    problems.append("%s: (offset, hash, fn) list differs from stage_bin_registrations.csv "
                                    "(%d vs %d entries, first diffs at %s)" % (name, len(got), len(regs[name]), diff))
                    status = "PAIRS MISMATCH"
            # resume points: entry + one per call (init + registrations, the tail is a `j` and has none)
            accepted = [a for a in range(BASE, BASE + len(data), 4) if accept_at(data, a)]
            want = 1 + len(s.resume)
            if len(accepted) != want:
                problems.append("%s: %d accepted entry addresses, expected %d" % (name, len(accepted), want))
        print("%-12s %6d %6d %9s %8d  %s" % (name, (s.end_pc // 4) if s.ok else 0, s.callbacks if s.ok else 0,
                                             ("0x%X" % s.end_pc) if s.ok else "-", len(s.resume) if s.ok else 0, status))

    print("total registrations: %d (stage_bins.csv: %d, expected %d)" % (total, csv_total, EXPECTED_TOTAL))
    if total != EXPECTED_TOTAL:
        problems.append("total registrations %d != %d" % (total, EXPECTED_TOTAL))
    if csv_total != EXPECTED_TOTAL:
        problems.append("stage_bins.csv registrations sum %d != %d" % (csv_total, EXPECTED_TOTAL))
    if regs:
        n_pairs = sum(len(v) for v in regs.values())
        print("stage_bin_registrations.csv rows: %d" % n_pairs)
        if n_pairs != EXPECTED_TOTAL:
            problems.append("stage_bin_registrations.csv has %d rows != %d" % (n_pairs, EXPECTED_TOTAL))

    if not args.no_cpp_check:
        cpp_problems = check_cpp(args.cpp)
        print("C++ constant parity (%s): %s" % (os.path.basename(args.cpp), "ok" if not cpp_problems else "MISMATCH"))
        problems += cpp_problems
    return problems, bins


# ------------------------------------------------------------------------------------------------------------
def poke(data, off, word):
    b = bytearray(data)
    struct.pack_into("<I", b, off, word)
    return bytes(b)


def self_test(bins):
    """Every mutation below must be REJECTED by the model (proves the whitelist is strict). Returns problems."""
    problems = []
    checks = 0

    def expect_reject(name, data, what):
        nonlocal checks
        checks += 1
        if scan_stub(data).ok:
            problems.append("self-test: %s: mutation accepted: %s" % (name, what))

    for name, data in bins.items():
        s = scan_stub(data)
        if not s.ok:
            continue
        ops = dict(s.ops)
        by_op = {}
        for off, op in s.ops:
            by_op.setdefault(op, []).append(off)
        end = s.end_pc
        tail = end - 8
        first_reg = by_op["jal_register"][0]
        mid_arg = [o for o in s.ops if o[1] in ARG_OPS][len(by_op["jal_register"]) // 2][0]
        last_call = by_op["jal_register"][-1]

        expect_reject(name, poke(data, 0, C["kWordNop"]), "prologue addiu sp replaced by nop")
        expect_reject(name, poke(data, 4, C["kWordNop"]), "prologue sd ra replaced by nop")
        expect_reject(name, poke(data, 8, C["kWordJalInit"] + 1), "jal init target changed")
        expect_reject(name, poke(data, 12, C["kWordJalRegister"]), "init delay slot is a call")
        expect_reject(name, poke(data, 0, 0x27BDFFE0), "addiu sp,sp,-0x20")
        expect_reject(name, poke(data, first_reg, C["kWordJalRegister"] + 1), "jal register target changed")
        expect_reject(name, poke(data, first_reg, C["kWordJalInit"]), "second jal to the init function")
        expect_reject(name, poke(data, first_reg, C["kWordJRegister"]), "early tail j (before ld ra)")
        expect_reject(name, poke(data, tail, C["kWordJRegister"] + 1), "tail j target changed")
        expect_reject(name, poke(data, tail, C["kWordJalRegister"]), "tail j turned into jal (runs into data)")
        expect_reject(name, poke(data, tail, 0x03E00009), "jalr instead of jr")
        expect_reject(name, poke(data, tail + 4, C["kWordNop"]), "terminator delay slot is nop (sp never restored)")
        expect_reject(name, poke(data, tail + 4, 0x27BD0020), "terminator delay slot restores wrong sp")
        expect_reject(name, poke(data, by_op["ld_ra"][0], C["kWordNop"]), "ld ra removed")
        expect_reject(name, poke(data, by_op["ld_ra"][0], 0xDFBF0008), "ld ra from the wrong slot")
        expect_reject(name, poke(data, first_reg + 8, C["kWordLdRa"]),
                      "ld ra before later calls (a later jal would clobber ra)")
        expect_reject(name, poke(data, mid_arg, 0xAC800000), "sw zero,0(a0) in the middle")
        expect_reject(name, poke(data, mid_arg, 0x10000000), "beq zero,zero in the middle")
        expect_reject(name, poke(data, mid_arg, 0x0000000C), "syscall in the middle")
        expect_reject(name, poke(data, mid_arg, 0x3C060000), "lui a2")
        expect_reject(name, poke(data, mid_arg, 0x34A50000), "ori a1,a1")
        expect_reject(name, poke(data, mid_arg, 0x24840000), "addiu a0,a0")
        expect_reject(name, poke(data, mid_arg, 0x3C1D0000), "lui sp")
        expect_reject(name, poke(data, mid_arg, C["kWordAddiuSpUp"]), "addiu sp,+0x10 in the middle")
        expect_reject(name, poke(data, mid_arg, C["kWordSdRa"]), "second sd ra")
        expect_reject(name, poke(data, mid_arg, C["kWordJrRa"]), "jr ra with the delay slot not restoring sp")
        expect_reject(name, poke(data, first_reg + 4, C["kWordJalRegister"]), "call in a call's delay slot")
        expect_reject(name, poke(data, first_reg + 4, 0xAC800000), "store in a call's delay slot")
        expect_reject(name, poke(data, last_call + 4, 0xFFFFFFFF), "junk in a call's delay slot")
        expect_reject(name, data[: end - 4], "truncated inside the tail")
        expect_reject(name, data[: end // 2], "truncated in the middle")

    # exhaustive poisoning of every code word of the smallest bin, with several non-whitelisted words
    smallest = min(bins, key=lambda k: len(bins[k]))
    s = scan_stub(bins[smallest])
    for poison in (0xFFFFFFFF, 0x00000001, 0x3C060000, 0xAFA40000):
        for off in range(0, s.end_pc, 4):
            expect_reject(smallest, poke(bins[smallest], off, poison), "poison 0x%08X at +0x%X" % (poison, off))

    print("self-test: %d mutations, %s" % (checks, "all rejected" if not problems else "%d ACCEPTED" % len(problems)))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bins", default=os.path.join(STAGES, "bins"), help="directory with the extracted <stage>.bin files")
    ap.add_argument("--csv", default=os.path.join(STAGES, "stage_bins.csv"))
    ap.add_argument("--regs", default=os.path.join(STAGES, "stage_bin_registrations.csv"))
    ap.add_argument("--cpp", default=os.path.join(ROOT, "src", "zoe_overrides.cpp"))
    ap.add_argument("--no-cpp-check", action="store_true", help="do not compare the k* constants with the C++ source")
    ap.add_argument("--self-test", action="store_true", help="also verify that mutated stubs are rejected")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    problems, bins = run_check(args)
    if args.self_test:
        problems += self_test(bins)

    if problems:
        print("\nFAILED:")
        for p in problems:
            print("  - " + p)
        return 1
    print("\nOK: all %d bins fully accepted, %d registrations" % (len(bins), EXPECTED_TOTAL))
    return 0


if __name__ == "__main__":
    sys.exit(main())
