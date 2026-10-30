// Headless helper: decompile functions and list xrefs.
// Args: <outfile> then any of: fn:<name|0xaddr>  xref:<0xaddr>
// @category PS2Recomp
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class DecompDump extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            DecompInterface di = new DecompInterface();
            di.openProgram(currentProgram);
            FunctionManager fm = currentProgram.getFunctionManager();
            for (int i = 1; i < args.length; i++) {
                String a = args[i];
                if (a.startsWith("fn:")) {
                    String k = a.substring(3);
                    Function f = null;
                    if (k.startsWith("0x")) {
                        f = fm.getFunctionContaining(toAddr(Long.parseLong(k.substring(2), 16)));
                    } else {
                        for (Function g : fm.getFunctions(true)) {
                            if (g.getName().equals(k)) { f = g; break; }
                        }
                    }
                    if (f == null) { out.println("// not found: " + k); continue; }
                    DecompileResults r = di.decompileFunction(f, 120, monitor);
                    out.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
                    out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
                } else if (a.startsWith("xref:")) {
                    Address t = toAddr(Long.parseLong(a.substring(7), 16));
                    out.println("// xrefs to " + t);
                    for (Reference r : getReferencesTo(t)) {
                        Function f = fm.getFunctionContaining(r.getFromAddress());
                        out.println("  " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (f == null ? "?" : f.getName()));
                    }
                }
            }
        }
    }
}
