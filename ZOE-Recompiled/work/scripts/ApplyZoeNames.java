// Applies the known ZOE function names to the Ghidra program (run BEFORE ExportPS2Functions.java,
// because the exporter turns runtime-known names (printf, sceDma*, ...) into [general].stubs).
//
// Inputs (../names relative to this script, unless script args give other CSV paths):
//   zoe_names.csv            address,name,source,confidence           -> every row is a function
//   zoe_names_cxx_rtti.csv   address,name,source,confidence,kind,note -> only kind == "function" rows
//                                                                        (vtable rows are data, skipped)
// For each address: rename the function that starts there, or create one (disassembling first) when
// Ghidra missed it. A row is skipped (and reported) when the address is not in an executable block or
// lies inside another function's body. Same name at two addresses (libc statics such as __sprint) gets
// a _<addr> suffix on the later one, since the exporter's TOML selectors are name@address anyway.
//
// Output: summary lines in the Ghidra log (prefix "ZOE-NAMES:") and <work>/names_applied.csv with one
// line per row (address,name,result,detail).
// @category PS2Recomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

public class ApplyZoeNames extends GhidraScript {

    private static final class Row {
        long addr;
        String name;
        String origin; // which CSV
    }

    private FunctionManager fm;
    private final Map<String, Integer> nameUse = new HashMap<>();
    private final List<String> report = new ArrayList<>();
    private final List<String> problems = new ArrayList<>();
    private int renamedFromDefault = 0, alreadyNamed = 0, renamedOverNamed = 0, createdAndNamed = 0, split = 0;
    private int notCode = 0, inside = 0, createFailed = 0, dupSuffixed = 0, renameFailed = 0;

    // Returns false only when the row lies inside another function and split is not allowed yet.
    private boolean apply(Row r, boolean allowSplit) throws Exception {
        Address a = toAddr(r.addr);
        MemoryBlock block = currentProgram.getMemory().getBlock(a);
        if (block == null || !block.isExecute()) {
            notCode++;
            report.add(String.format("0x%08X,%s,SKIP_NOT_CODE,%s", r.addr, r.name, block == null ? "no block" : block.getName()));
            problems.add(String.format("not code: 0x%08X %s", r.addr, r.name));
            return true;
        }

        String name = r.name;
        if (nameUse.getOrDefault(name, 0) > 0) {
            name = String.format("%s_%08x", r.name, r.addr);
            dupSuffixed++;
        }

        Function f = fm.getFunctionAt(a);
        boolean createdNow = false;
        String how = "CREATED";
        if (f == null) {
            Function containing = fm.getFunctionContaining(a);
            if (containing != null) {
                if (!allowSplit) {
                    if (name != r.name) {
                        dupSuffixed--; // counted again when the row is retried
                    }
                    return false;
                }
                // Remove the swallowing function, create the row's function first, then give the old one
                // back (its body now stops at the new function's entry).
                Address ce = containing.getEntryPoint();
                String cn = containing.getName();
                boolean cDefault = containing.getSymbol().getSource() == SourceType.DEFAULT;
                String oldBody = containing.getBody().toString();
                removeFunction(containing);
                if (getInstructionAt(a) == null) {
                    disassemble(a);
                }
                try {
                    f = createFunction(a, name);
                } catch (Exception e) {
                    f = null;
                }
                Function cf = null;
                try {
                    cf = createFunction(ce, cDefault ? null : cn);
                } catch (Exception e) {
                    cf = null;
                }
                if (f == null || cf == null) {
                    inside++;
                    report.add(String.format("0x%08X,%s,SPLIT_FAILED,container %s@%s new=%s container=%s", r.addr, r.name, cn, ce, f != null, cf != null));
                    problems.add(String.format("split failed: 0x%08X %s (container %s@%s recreated=%s)", r.addr, r.name, cn, ce, cf != null));
                    if (f == null) {
                        return true;
                    }
                } else {
                    split++;
                    println(String.format("ZOE-NAMES: split %s@%s out of %s@%s: container body was %s, now %s; new body %s",
                        name, a, cn, ce, oldBody, cf.getBody(), f.getBody()));
                }
                createdNow = true;
                how = "CREATED_SPLIT";
            } else {
                if (getInstructionAt(a) == null) {
                    disassemble(a);
                }
                try {
                    f = createFunction(a, name);
                } catch (Exception e) {
                    f = null;
                }
                if (f == null) {
                    createFailed++;
                    report.add(String.format("0x%08X,%s,CREATE_FAILED,", r.addr, r.name));
                    problems.add(String.format("create failed: 0x%08X %s", r.addr, r.name));
                    return true;
                }
                createdNow = true;
            }
        }

        String old = f.getName();
        boolean oldDefault = f.getSymbol().getSource() == SourceType.DEFAULT;
        if (!old.equals(name)) {
            try {
                f.setName(name, SourceType.USER_DEFINED);
            } catch (Exception e) {
                renameFailed++;
                report.add(String.format("0x%08X,%s,RENAME_FAILED,%s", r.addr, name, e.getMessage()));
                problems.add(String.format("rename failed: 0x%08X %s (%s)", r.addr, name, e));
                return true;
            }
        }
        nameUse.merge(r.name, 1, Integer::sum);
        if (createdNow) {
            createdAndNamed++;
            report.add(String.format("0x%08X,%s,%s,", r.addr, name, how));
        } else if (old.equals(name)) {
            alreadyNamed++;
            report.add(String.format("0x%08X,%s,ALREADY_NAMED,", r.addr, name));
        } else if (oldDefault) {
            renamedFromDefault++;
            report.add(String.format("0x%08X,%s,RENAMED,was %s", r.addr, name, old));
        } else {
            renamedOverNamed++;
            report.add(String.format("0x%08X,%s,RENAMED_OVER_NAMED,was %s", r.addr, name, old));
        }
        return true;
    }

    @Override
    public void run() throws Exception {
        File namesDir = new File(getSourceFile().getFile(false).getParentFile().getParentFile(), "names");
        List<File> csvFiles = new ArrayList<>();
        String[] args = getScriptArgs();
        if (args != null && args.length > 0) {
            for (String a : args) {
                csvFiles.add(new File(a));
            }
        } else {
            csvFiles.add(new File(namesDir, "zoe_names.csv"));
            csvFiles.add(new File(namesDir, "zoe_names_cxx_rtti.csv"));
        }
        File reportFile = new File(namesDir.getParentFile(), "names_applied.csv");

        // address -> row (first file wins on the same address; a differing name is reported)
        Map<Long, Row> rows = new TreeMap<>();
        List<String> conflicts = new ArrayList<>();
        for (File f : csvFiles) {
            int read = 0, skippedKind = 0;
            try (BufferedReader br = new BufferedReader(new FileReader(f))) {
                String header = br.readLine();
                if (header == null) {
                    throw new IllegalStateException("empty CSV " + f);
                }
                String[] cols = header.split(",", -1);
                int kindCol = -1;
                for (int i = 0; i < cols.length; i++) {
                    if (cols[i].trim().equals("kind")) {
                        kindCol = i;
                    }
                }
                String line;
                while ((line = br.readLine()) != null) {
                    if (line.trim().isEmpty()) {
                        continue;
                    }
                    String[] p = line.split(",", Math.max(cols.length, 6));
                    if (kindCol >= 0 && kindCol < p.length && !p[kindCol].trim().equals("function")) {
                        skippedKind++;
                        continue;
                    }
                    Row r = new Row();
                    r.addr = Long.decode(p[0].trim()) & 0xFFFFFFFFL;
                    r.name = p[1].trim();
                    r.origin = f.getName();
                    read++;
                    Row prev = rows.get(r.addr);
                    if (prev == null) {
                        rows.put(r.addr, r);
                    } else if (!prev.name.equals(r.name)) {
                        conflicts.add(String.format("0x%08X %s (%s) vs %s (%s)", r.addr, prev.name, prev.origin, r.name, r.origin));
                    }
                }
            }
            println(String.format("ZOE-NAMES: %s: %d function rows read, %d non-function rows skipped", f.getName(), read, skippedKind));
        }

        fm = currentProgram.getFunctionManager();
        int before = fm.getFunctionCount();

        // Pass 1 handles every row that is not inside another function's body. Rows that are (Ghidra
        // swallowed a real function into its caller, typically via a tail jump) are split out in pass 2.
        List<Row> deferred = new ArrayList<>();
        for (Row r : rows.values()) {
            if (monitor.isCancelled()) {
                break;
            }
            if (!apply(r, false)) {
                deferred.add(r);
            }
        }
        for (Row r : deferred) {
            if (monitor.isCancelled()) {
                break;
            }
            apply(r, true);
        }

        int afterApply = fm.getFunctionCount();
        if (createdAndNamed > 0 && !monitor.isCancelled()) {
            // let Ghidra follow the flow of the functions we just created (new callees etc.)
            analyzeChanges(currentProgram);
        }
        int afterAnalysis = fm.getFunctionCount();

        int landed = renamedFromDefault + alreadyNamed + renamedOverNamed + createdAndNamed;
        try (PrintWriter w = new PrintWriter(reportFile)) {
            w.println("address,name,result,detail");
            for (String l : report) {
                w.println(l);
            }
        }
        println(String.format("ZOE-NAMES: %d unique addresses; landed %d (renamed from default %d, already named %d, renamed over another name %d, created %d of which %d split out of a swallowing function)",
            rows.size(), landed, renamedFromDefault, alreadyNamed, renamedOverNamed, createdAndNamed, split));
        println(String.format("ZOE-NAMES: not landed: %d not-in-code, %d split problems (row inside another function), %d create failed, %d rename failed; %d duplicate names suffixed",
            notCode, inside, createFailed, renameFailed, dupSuffixed));
        println(String.format("ZOE-NAMES: function count %d before, %d after applying names, %d after follow-up analysis of created functions",
            before, afterApply, afterAnalysis));
        for (String c : conflicts) {
            println("ZOE-NAMES: conflict (same address, different name): " + c);
        }
        int shown = 0;
        for (String p : problems) {
            if (shown++ >= 40) {
                println("ZOE-NAMES: ... " + (problems.size() - 40) + " more problems, see " + reportFile);
                break;
            }
            println("ZOE-NAMES: problem: " + p);
        }
        println("ZOE-NAMES: per-row report: " + reportFile.getAbsolutePath());
    }
}
