"""Resolve an in-process profile (profile.txt from F10) against the MSVC linker map.

usage: profile_report.py <profile.txt> <ps2EntryRunner.map> [top=40]
Prints per-thread totals, then the hottest functions per thread and grouped categories.
"""
import bisect
import collections
import re
import sys


def load_map(path):
    syms = []
    pat = re.compile(r"^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+[f ]?\s*\S*", re.I)
    base = None
    for line in open(path, errors="ignore"):
        if base is None and "Preferred load address is" in line:
            base = int(line.split()[-1], 16)
        m = pat.match(line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    return base, syms


def undecorate(name):
    m = re.match(r"\?([^@]+)@", name)
    if m:
        return m.group(1)
    return name.lstrip("_")


def category(name):
    n = name
    if re.search(r"_0x[0-9a-f]+$", n):
        return "guest: recompiled game code"
    for key, cat in (("GSCpuBackend", "GS software rasterizer"), ("VU1Interpreter", "VU1 interpreter"),
                     ("GS@@", "GS frontend"), ("processVIF1", "VIF1"), ("PS2Memory", "memory/DMA"),
                     ("EeScheduler", "EE scheduler"), ("lookupFunction", "dispatch"), ("dispatchGuestBranch", "dispatch"),
                     ("findOverlayFunction", "dispatch"), ("hasFunction", "dispatch"), ("ps2_log", "logging"),
                     ("basic_ostream", "logging (iostream)"), ("Mpeg", "MPEG"), ("sws_", "MPEG"), ("ff_", "MPEG")):
        if key in n:
            return cat
    if n.startswith("<dll>"):
        return n
    return "other: " + undecorate(n)[:40]


def main():
    prof, mapfile = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    base, syms = load_map(mapfile)
    addrs = [a for a, _ in syms]
    per_thread = collections.Counter()
    per_func = collections.defaultdict(collections.Counter)
    per_cat = collections.defaultdict(collections.Counter)
    for line in open(prof):
        if line.startswith("#"):
            continue
        parts = line.split()
        tid, kind, rip, n = parts[:4]
        tid, n = int(tid), int(n)
        per_thread[tid] += n
        if kind == "rva":
            va = base + int(rip, 16)
            i = bisect.bisect_right(addrs, va) - 1
            name = syms[i][1] if i >= 0 else "?"
        else:
            name = "<dll> " + (parts[4] if len(parts) > 4 else "?")
        per_func[tid][name] += n
        per_cat[tid][category(name)] += n
    total = sum(per_thread.values())
    busy = {t: sum(c for nm, c in per_func[t].items() if not nm.startswith("<dll>")) for t in per_thread}
    for tid, n in per_thread.most_common():
        if busy[tid] < n * 0.05:
            continue  # thread spends >95% in system DLLs: idle/waiting
        print(f"=== thread {tid}: {n} samples ({100.0 * n / total:.1f}% of all)")
        for cat, c in per_cat[tid].most_common(12):
            print(f"   {100.0 * c / n:5.1f}%  {cat}")
        print("   -- top functions --")
        for name, c in per_func[tid].most_common(top):
            print(f"   {100.0 * c / n:5.1f}%  {undecorate(name)[:90]}")


if __name__ == "__main__":
    main()
