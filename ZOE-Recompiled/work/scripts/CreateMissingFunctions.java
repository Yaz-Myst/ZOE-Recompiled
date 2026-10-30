// Creates functions for code that Ghidra's flow analysis never reached (typically C++ virtual methods
// and callbacks that are only referenced from data), so the recompiler sees them at export time.
// Run AFTER ApplyZoeNames.java and BEFORE ExportPS2Functions.java (see GHIDRA_PRE_EXPORT_SCRIPTS).
//
// A region is "uncovered" when it is executable, in the default address space (DVP overlay blocks are
// never touched) and not inside any function body. Candidate targets come from
//   1. vtable rows of names/zoe_names_cxx_rtti.csv: entries are 8 bytes {delta, pfn}, pfn at +4, the
//      row's note gives the entry count ("N entries (slot 0 = __tf getter ...)");
//   2. every other aligned data word (all initialized non-executable blocks) that holds an address;
//   3. lui + addiu/ori/daddiu address pairs in code that is already covered by a function;
//   4. names/zoe_stage_bin_ptrs.csv (tools/extract_stage_bin_ptrs.py): main-ELF addresses that the
//      run-time loaded per-stage <stage>.bin code registers as callbacks or calls (jal). Nothing inside the
//      ELF refers to them.
// A target is accepted only if it is 4-aligned, executable, inside an uncovered region and looks like a
// function start: (a) first word of its uncovered region, (b) preceded by `jr ra` + delay slot, or
// (c) its first instruction is addiu/daddiu sp,sp,-N (nop padding between is tolerated in (a)/(b)).
// Sources 1 and 4 are real function pointers (vtable slots, registered callbacks), so for them the
// signature test is waived and only "aligned, executable, inside an uncovered region" is required (some
// leaf methods start without prologue right after a tail `j`); the generic data words of source 2 also
// include switch-table entries, so they always need a signature. Rejections are reported.
// After each round Ghidra analysis follows the new functions (callees, ...) and the scan repeats until
// nothing new is found (new code exposes more lui pairs). A created function is removed again if its
// body leaves executable memory or is implausibly large (> MAX_FUNCTION_BYTES).
//
// Output: "ZOE-MISSING:" lines in the Ghidra log, <work>/missing_functions.csv (one row per created or
// rejected candidate) and <work>/uncovered_leftovers.csv (uncovered regions that still contain a
// `jr ra`, with a one-line reason each).
// @category PS2Recomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.MemoryBlock;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class CreateMissingFunctions extends GhidraScript {

    private static final long MASK = 0xFFFFFFFFL;
    private static final long JR_RA = 0x03E00008L;
    private static final long MAX_FUNCTION_BYTES = 0x10000L;
    private static final int MAX_ROUNDS = 12;

    private static final class Range {
        long start;   // inclusive
        long end;     // exclusive
        int[] words;
    }

    private static final class Cand {
        long target;
        List<String> sources = new ArrayList<>();
        boolean auth = false;   // vtable slot / stage-code callback pointer: a real function pointer, start signature not required
        String result = "";
    }

    private final List<Range> execRanges = new ArrayList<>();
    private final Map<Long, Cand> cands = new TreeMap<>();
    private FunctionManager fm;
    private AddressSet exec;
    private boolean sweep = false;

    private Long word(long addr) {
        for (Range r : execRanges) {
            if (addr >= r.start && addr + 4 <= r.end) {
                return r.words[(int) ((addr - r.start) >>> 2)] & MASK;
            }
        }
        return null;
    }

    private void addCand(long target, String source) {
        Cand c = cands.get(target);
        if (c == null) {
            c = new Cand();
            c.target = target;
            cands.put(target, c);
        }
        if (source.startsWith("stagebin") || source.startsWith("vtable")) {
            c.auth = true;
        }
        if (c.sources.size() < 4 && !c.sources.contains(source)) {
            c.sources.add(source);
        }
    }

    private boolean inExec(long a) {
        for (Range r : execRanges) {
            if (a >= r.start && a < r.end) {
                return true;
            }
        }
        return false;
    }

    private static boolean isSpAdjustDown(long w) {
        int op = (int) (w >>> 26);
        int rs = (int) ((w >>> 21) & 31);
        int rt = (int) ((w >>> 16) & 31);
        return (op == 0x09 || op == 0x19) && rs == 29 && rt == 29 && (w & 0x8000L) != 0;
    }

    private static boolean isJumpTerminator(long w) {
        int op = (int) (w >>> 26);
        return op == 2 || (op == 0 && (w & 0x3F) == 0x08);   // j, or jr (ra or any register)
    }

    private boolean bodyHasTerminator(Function f) {
        for (AddressRange rg : f.getBody()) {
            for (long a = rg.getMinAddress().getOffset(); a <= rg.getMaxAddress().getOffset(); a += 4) {
                Long w = word(a);
                if (w != null && isJumpTerminator(w)) {
                    return true;
                }
            }
        }
        return false;
    }

    private static boolean isJrRa(long w) {
        return w == JR_RA;
    }

    private static boolean writesReg(long w, int reg) {
        int op = (int) (w >>> 26);
        int rt = (int) ((w >>> 16) & 31);
        int rd = (int) ((w >>> 11) & 31);
        if (op == 0 || op == 0x1C) {
            return rd == reg;
        }
        if (op == 3) {
            return reg == 31;
        }
        if ((op >= 0x08 && op <= 0x0F) || op == 0x18 || op == 0x19 || (op >= 0x20 && op <= 0x27)
            || op == 0x1A || op == 0x1B || op == 0x1E || op == 0x37) {
            return rt == reg;
        }
        return false;
    }

    private static boolean isControlTransfer(long w) {
        int op = (int) (w >>> 26);
        if (op == 0) {
            int fn = (int) (w & 0x3F);
            return fn == 0x08 || fn == 0x09;
        }
        return op == 1 || op == 2 || op == 3 || (op >= 4 && op <= 7) || (op >= 0x14 && op <= 0x17);
    }

    // null when the target is acceptable; otherwise the reason it is not.
    private String rejectReason(long t, AddressSetView unc, String[] how, boolean auth) {
        if ((t & 3) != 0) {
            return "not 4-aligned";
        }
        if (!inExec(t)) {
            return "not in executable memory";
        }
        Address ta = toAddr(t);
        AddressRange region = unc.getRangeContaining(ta);
        if (region == null) {
            return fm.getFunctionAt(ta) != null ? "already a function" : "already inside a function";
        }
        Long w0 = word(t);
        if (w0 == null) {
            return "unreadable";
        }
        if (w0 == 0) {
            return "target is a nop (alignment padding)";
        }
        // (a) first word of the uncovered region, allowing nop padding before the target
        long rs = region.getMinAddress().getOffset();
        boolean onlyPadding = true;
        for (long a = rs; a < t; a += 4) {
            Long pw = word(a);
            if (pw == null || pw != 0) {
                onlyPadding = false;
                break;
            }
        }
        if (onlyPadding) {
            how[0] = "region start";
            return null;
        }
        // (b) preceded by jr ra + delay slot, with up to 7 words of nop padding in between
        for (int k = 0; k <= 7; k++) {
            long p = t - 4L * k;
            Long pjr = word(p - 8);
            if (pjr != null && isJrRa(pjr)) {
                boolean pad = true;
                for (long a = p; a < t; a += 4) {
                    Long pw = word(a);
                    if (pw == null || pw != 0) {
                        pad = false;
                        break;
                    }
                }
                if (pad) {
                    how[0] = k == 0 ? "after jr ra" : "after jr ra + padding";
                    return null;
                }
            }
        }
        // (c) prologue
        if (isSpAdjustDown(w0)) {
            how[0] = "addiu sp prologue";
            return null;
        }
        if (auth) {
            how[0] = "trusted pointer (vtable/stage code), no start signature";
            return null;
        }
        return "no function-start signature";
    }

    @Override
    public void run() throws Exception {
        File namesDir = new File(getSourceFile().getFile(false).getParentFile().getParentFile(), "names");
        File workDir = namesDir.getParentFile();
        fm = currentProgram.getFunctionManager();
        String[] scriptArgs = getScriptArgs();
        for (String a : scriptArgs == null ? new String[0] : scriptArgs) {
            if (a.equals("sweep")) {
                sweep = true;
            }
        }
        println("ZOE-MISSING: mode = " + (sweep ? "pointer sources + boundary sweep" : "pointer sources only"));
        AddressSpace dflt = currentProgram.getAddressFactory().getDefaultAddressSpace();

        exec = new AddressSet();
        List<MemoryBlock> dataBlocks = new ArrayList<>();
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.isOverlay() || b.getStart().getAddressSpace() != dflt || !b.isInitialized()) {
                continue;
            }
            if (b.isExecute()) {
                Range r = new Range();
                r.start = b.getStart().getOffset();
                r.end = b.getEnd().getOffset() + 1;
                byte[] raw = new byte[(int) b.getSize()];
                b.getBytes(b.getStart(), raw);
                int n = raw.length / 4;
                r.words = new int[n];
                ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN).asIntBuffer().get(r.words);
                execRanges.add(r);
                exec.add(b.getStart(), b.getEnd());
            } else {
                dataBlocks.add(b);
            }
        }
        if (execRanges.isEmpty()) {
            println("ZOE-MISSING: no executable block, nothing to do");
            return;
        }
        println("ZOE-MISSING: executable ranges: " + execRanges.size() + ", data blocks scanned: " + dataBlocks.size());

        // static candidates: vtable entries first (so they lead the source list), then plain data words
        int vtRows = 0, vtEntries = 0;
        File rtti = new File(namesDir, "zoe_names_cxx_rtti.csv");
        if (rtti.exists()) {
            try (BufferedReader br = new BufferedReader(new FileReader(rtti))) {
                br.readLine();
                String line;
                while ((line = br.readLine()) != null) {
                    String[] p = line.split(",", 6);
                    if (p.length < 6 || !p[4].trim().equals("vtable")) {
                        continue;
                    }
                    long va = Long.decode(p[0].trim()) & MASK;
                    int n = 0;
                    try {
                        n = Integer.parseInt(p[5].trim().split(" ")[0]);
                    } catch (Exception e) {
                        n = 0;
                    }
                    vtRows++;
                    for (int k = 0; k < n; k++) {
                        try {
                            long pfn = currentProgram.getMemory().getInt(toAddr(va + 8L * k + 4)) & MASK;
                            if (pfn != 0) {
                                addCand(pfn, "vtable:" + p[1].trim() + "#" + k);
                                vtEntries++;
                            }
                        } catch (Exception e) {
                            break;
                        }
                    }
                }
            }
        }
        int stageRows = 0;
        File stageCsv = new File(namesDir, "zoe_stage_bin_ptrs.csv");
        if (stageCsv.exists()) {
            try (BufferedReader br = new BufferedReader(new FileReader(stageCsv))) {
                br.readLine();
                String line;
                while ((line = br.readLine()) != null) {
                    String[] p = line.split(",", 4);
                    if (p.length < 2 || p[0].trim().isEmpty()) {
                        continue;
                    }
                    addCand(Long.decode(p[0].trim()) & MASK, "stagebin:" + p[1].trim());
                    stageRows++;
                }
            }
        }
        println("ZOE-MISSING: " + stageRows + " stage .bin pointers read from " + stageCsv.getName() + (stageCsv.exists() ? "" : " (missing: run tools/extract_stage_bin_ptrs.py)"));
        int dataHits = 0;
        for (MemoryBlock b : dataBlocks) {
            byte[] raw = new byte[(int) b.getSize()];
            b.getBytes(b.getStart(), raw);
            ByteBuffer bb = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
            long base = b.getStart().getOffset();
            for (int off = 0; off + 4 <= raw.length; off += 4) {
                long v = bb.getInt(off) & MASK;
                if ((v & 3) == 0 && inExec(v)) {
                    addCand(v, "data:" + Long.toHexString(base + off));
                    dataHits++;
                }
            }
        }
        println(String.format("ZOE-MISSING: %d vtable rows / %d vtable entries; %d data words point into executable memory; %d distinct static targets",
            vtRows, vtEntries, dataHits, cands.size()));

        Map<String, Integer> createdBySource = new LinkedHashMap<>();
        int created = 0, removed = 0, createdNoSig = 0;
        int regionsBefore = -1;
        List<String> report = new ArrayList<>();
        Map<Long, String> luiTargets = new TreeMap<>();

        for (int round = 1; round <= MAX_ROUNDS && !monitor.isCancelled(); round++) {
            AddressSet covered = new AddressSet();
            for (Function f : fm.getFunctions(true)) {
                covered.add(f.getBody());
            }
            AddressSet unc = new AddressSet(exec.subtract(covered));

            // dynamic candidates: lui + addiu/ori pairs in covered code
            for (Range r : execRanges) {
                int n = r.words.length;
                for (int i = 0; i < n; i++) {
                    long w = r.words[i] & MASK;
                    if ((w >>> 26) != 0x0F) {
                        continue;
                    }
                    int reg = (int) ((w >>> 16) & 31);
                    if (reg == 0) {
                        continue;
                    }
                    long addr = r.start + 4L * i;
                    if (!covered.contains(toAddr(addr))) {
                        continue;
                    }
                    long upper = (w & 0xFFFFL) << 16;
                    boolean sawCt = false;
                    for (int la = 1; la <= 6 && i + la < n; la++) {
                        long lw = r.words[i + la] & MASK;
                        int op = (int) (lw >>> 26);
                        int rs = (int) ((lw >>> 21) & 31);
                        if ((op == 0x09 || op == 0x0D || op == 0x19) && rs == reg) {
                            long imm = lw & 0xFFFFL;
                            long target = (op == 0x0D) ? (upper | imm) : ((upper + (short) imm) & MASK);
                            if ((target & 3) == 0 && inExec(target)) {
                                if (!luiTargets.containsKey(target)) {
                                    luiTargets.put(target, Long.toHexString(addr + 4L * la));
                                }
                            }
                            break;
                        }
                        if (sawCt) {
                            break;
                        }
                        if (writesReg(lw, reg)) {
                            break;
                        }
                        if (isControlTransfer(lw)) {
                            sawCt = true;
                        }
                    }
                }
            }
            for (Map.Entry<Long, String> e : luiTargets.entrySet()) {
                addCand(e.getKey(), "lui:" + e.getValue());
            }

            int madeThisRound = 0;
            for (Cand c : new ArrayList<>(cands.values())) {
                if (monitor.isCancelled()) {
                    break;
                }
                if (c.result.startsWith("created")) {
                    continue;
                }
                // regions change while we create functions, so recompute per candidate from the manager
                Address ta = toAddr(c.target);
                if ((c.target & 3) == 0 && inExec(c.target) && fm.getFunctionContaining(ta) != null) {
                    if (!c.result.startsWith("removed")) {
                        c.result = fm.getFunctionAt(ta) != null ? "already a function" : "already inside a function";
                    }
                    continue;
                }
                String[] how = new String[1];
                String why = rejectReason(c.target, unc, how, c.auth);
                if (why != null) {
                    c.result = "rejected: " + why;
                    continue;
                }
                if (getInstructionAt(ta) == null) {
                    disassemble(ta);
                }
                Function f = null;
                try {
                    f = createFunction(ta, null);
                } catch (Exception e) {
                    f = null;
                }
                if (f == null) {
                    c.result = "rejected: createFunction failed (no valid code)";
                    continue;
                }
                long size = f.getBody().getNumAddresses();
                boolean outside = !exec.contains(f.getBody());
                if (outside || size > MAX_FUNCTION_BYTES) {
                    fm.removeFunction(f.getEntryPoint());
                    removed++;
                    c.result = "removed: " + (outside ? "body leaves executable memory" : "body too large (" + size + " bytes)");
                    continue;
                }
                unc.delete(f.getBody());
                created++;
                madeThisRound++;
                if (how[0].startsWith("trusted pointer")) {
                    createdNoSig++;
                }
                c.result = "created (" + how[0] + ", round " + round + ")";
                String key = c.sources.get(0).split(":")[0];
                createdBySource.merge(key, 1, Integer::sum);
            }
            println(String.format("ZOE-MISSING: round %d: created %d functions (total %d, removed %d)", round, madeThisRound, created, removed));
            if (madeThisRound == 0) {
                break;
            }
            analyzeChanges(currentProgram);
        }

        // Optional boundary sweep (script arg "sweep"): uncovered regions that still hold a `jr ra` and start
        // at a natural boundary (region start or right after a return + delay slot, nop padding skipped) are
        // turned into functions even when nothing points at them (boundaries: after `jr ra` or a tail `j`, each
        // + delay slot). .text of this ELF is pure code (verified: no strings or pointer tables), so this
        // cannot pick up data; a function is dropped again if its body has no jr/j, leaves executable memory
        // or is too large.
        int sweepCreated = 0;
        if (sweep) {
            java.util.Set<Long> tried = new java.util.HashSet<>();
            for (int round = 1; round <= MAX_ROUNDS && !monitor.isCancelled(); round++) {
                AddressSet unc = new AddressSet(exec.subtract(coveredNow()));
                List<Long> starts = new ArrayList<>();
                for (AddressRange rg : unc) {
                    long s0 = rg.getMinAddress().getOffset();
                    long e0 = rg.getMaxAddress().getOffset() + 1;
                    boolean hasJr = false;
                    for (long a = s0; a < e0; a += 4) {
                        Long w = word(a);
                        if (w != null && isJrRa(w)) {
                            hasJr = true;
                            break;
                        }
                    }
                    if (!hasJr) {
                        continue;
                    }
                    long a = s0;
                    while (a < e0 && word(a) != null && word(a) == 0) {
                        a += 4;
                    }
                    if (a < e0) {
                        starts.add(a);
                    }
                    for (long j = a; j < e0; j += 4) {
                        Long w = word(j);
                        if (w != null && (isJrRa(w) || (w >>> 26) == 2)) {
                            long p = j + 8;
                            while (p < e0 && word(p) != null && word(p) == 0) {
                                p += 4;
                            }
                            if (p < e0) {
                                starts.add(p);
                            }
                        }
                    }
                }
                int made = 0;
                for (long t : starts) {
                    if (monitor.isCancelled()) {
                        break;
                    }
                    Address ta = toAddr(t);
                    if (tried.contains(t) || !unc.contains(ta)) {
                        continue;
                    }
                    tried.add(t);
                    Cand c = cands.get(t);
                    if (c == null) {
                        c = new Cand();
                        c.target = t;
                        cands.put(t, c);
                    }
                    c.sources.add("sweep");
                    if (getInstructionAt(ta) == null) {
                        disassemble(ta);
                    }
                    Function f = null;
                    try {
                        f = createFunction(ta, null);
                    } catch (Exception e) {
                        f = null;
                    }
                    if (f == null) {
                        c.result = "rejected: sweep: createFunction failed (no valid code)";
                        continue;
                    }
                    long size = f.getBody().getNumAddresses();
                    boolean hasRet = bodyHasTerminator(f);
                    boolean outside = !exec.contains(f.getBody());
                    if (outside || size > MAX_FUNCTION_BYTES || !hasRet) {
                        fm.removeFunction(f.getEntryPoint());
                        removed++;
                        c.result = "removed: sweep: " + (outside ? "body leaves executable memory" : size > MAX_FUNCTION_BYTES ? "body too large" : "no jr/j in body");
                        continue;
                    }
                    unc.delete(f.getBody());
                    created++;
                    sweepCreated++;
                    made++;
                    c.result = "created (sweep, round " + round + ")";
                }
                println(String.format("ZOE-MISSING: sweep round %d: created %d functions (sweep total %d)", round, made, sweepCreated));
                if (made == 0) {
                    break;
                }
                analyzeChanges(currentProgram);
            }
            createdBySource.merge("sweep", sweepCreated, Integer::sum);
        }

        // report
        for (Cand c : cands.values()) {
            if (c.result.isEmpty() || c.result.startsWith("already")) {
                continue;
            }
            report.add(String.format("0x%08X,\"%s\",%s", c.target, c.result, String.join("|", c.sources)));
        }
        try (PrintWriter w = new PrintWriter(new File(workDir, "missing_functions.csv"))) {
            w.println("address,result,sources");
            for (String l : report) {
                w.println(l);
            }
        }

        // raw jal/j targets anywhere in .text (also from uncovered code) -> first caller, for the reasons
        TreeMap<Long, Long> rawCall = new TreeMap<>();
        for (Range r : execRanges) {
            for (int i = 0; i < r.words.length; i++) {
                long w = r.words[i] & MASK;
                int op = (int) (w >>> 26);
                if (op == 2 || op == 3) {
                    long t = ((r.start + 4L * i) & 0xF0000000L) | ((w & 0x3FFFFFFL) << 2);
                    if (!rawCall.containsKey(t)) {
                        rawCall.put(t, r.start + 4L * i);
                    }
                }
            }
        }

        // leftovers: uncovered regions that still contain a jr ra
        AddressSet coveredEnd = coveredNow();
        AddressSetView uncEnd = exec.subtract(coveredEnd);
        List<String> left = new ArrayList<>();
        int regionCount = 0, regionsWithJr = 0, wordsInJr = 0, jrCount = 0;
        Map<String, Integer> reasons = new LinkedHashMap<>();
        for (AddressRange rg : uncEnd) {
            regionCount++;
            long s = rg.getMinAddress().getOffset();
            long e = rg.getMaxAddress().getOffset() + 1;
            int jr = 0;
            for (long a = s; a < e; a += 4) {
                Long w = word(a);
                if (w != null && isJrRa(w)) {
                    jr++;
                }
            }
            if (jr == 0) {
                continue;
            }
            regionsWithJr++;
            jrCount += jr;
            wordsInJr += (int) ((e - s) / 4);
            // reason: what points into the region, and what precedes it
            int inbound = 0;
            String firstReject = "";
            String sweepNote = "";
            for (Cand c : cands.values()) {
                if (c.target >= s && c.target < e && c.sources.size() == 1 && c.sources.get(0).equals("sweep")) {
                    if (sweepNote.isEmpty() && !c.result.startsWith("created")) {
                        sweepNote = String.format("sweep at 0x%X: %s", c.target, c.result);
                    }
                    continue;
                }
                if (c.target >= s && c.target < e) {
                    inbound++;
                    if (firstReject.isEmpty() && c.result.startsWith("rejected")) {
                        firstReject = String.format("0x%X %s", c.target, c.result.substring(10));
                    } else if (firstReject.isEmpty() && c.result.startsWith("removed")) {
                        firstReject = String.format("0x%X %s", c.target, c.result);
                    }
                }
            }
            String reason;
            Long pw = word(s - 4);
            Long pw2 = word(s - 8);
            String calledFrom = "";
            for (Map.Entry<Long, Long> rc : rawCall.subMap(s, e).entrySet()) {
                boolean callerUncovered = uncEnd.contains(toAddr(rc.getValue()));
                calledFrom = String.format("jal/j from 0x%X (%s)", rc.getValue(), callerUncovered ? "itself uncovered" : "covered code, target mid-function");
                break;
            }
            if (!calledFrom.isEmpty() && inbound == 0) {
                reason = "only " + calledFrom;
            } else if (inbound > 0) {
                reason = inbound + " pointer(s) into region, none accepted (" + firstReject + ")";
            } else if (!sweepNote.isEmpty()) {
                reason = "no pointer; " + sweepNote;
            } else if (pw2 != null && isJrRa(pw2)) {
                reason = "no reference of any kind; follows a return (unreferenced/dead function?)";
            } else if (pw2 != null && (pw2 >>> 26) == 0 && (pw2 & 0x3F) == 0x08) {
                reason = "no reference; follows a computed jr (switch-case code of the previous function?)";
            } else if (pw != null && (pw >>> 26) == 2) {
                reason = "no reference; follows a j (tail jump / unreachable tail?)";
            } else {
                reason = "no reference of any kind";
            }
            reasons.merge(reason.replaceAll("[0-9]+ pointer.s. into region, none accepted .*", "pointer(s) into region, none accepted").replaceAll("0x[0-9A-Fa-f]+", "0x_"), 1, Integer::sum);
            left.add(String.format("0x%08X,%d,%d,\"%s\"", s, (e - s) / 4, jr, reason));
        }
        try (PrintWriter w = new PrintWriter(new File(workDir, "uncovered_leftovers.csv"))) {
            w.println("start,words,jr_ra_count,reason");
            for (String l : left) {
                w.println(l);
            }
        }

        println(String.format("ZOE-MISSING: created %d functions (by first source: %s; %d of them only accepted because a vtable slot or stage-code callback points at them (no start signature)), %d removed as implausible; %d candidates checked", created, createdBySource, createdNoSig, removed, cands.size()));
        println(String.format("ZOE-MISSING: uncovered regions left: %d, of which %d contain a jr ra (%d jr ra, %d words)", regionCount, regionsWithJr, jrCount, wordsInJr));
        for (Map.Entry<String, Integer> e : reasons.entrySet()) {
            println("ZOE-MISSING: leftover reason x" + e.getValue() + ": " + e.getKey());
        }
        int shown = 0;
        for (String l : left) {
            if (shown++ >= 60) {
                println("ZOE-MISSING: ... more leftovers in " + new File(workDir, "uncovered_leftovers.csv"));
                break;
            }
            println("ZOE-MISSING: leftover " + l);
        }
        println("ZOE-MISSING: reports: " + new File(workDir, "missing_functions.csv") + ", " + new File(workDir, "uncovered_leftovers.csv"));
    }

    private AddressSet coveredNow() {
        AddressSet covered = new AddressSet();
        for (Function f : fm.getFunctions(true)) {
            covered.add(f.getBody());
        }
        return covered;
    }
}
