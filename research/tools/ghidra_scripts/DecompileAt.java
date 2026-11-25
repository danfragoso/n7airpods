// DecompileAt.java — decompile a handler, its callees and callers, and write C out.
// Addresses come from build/decompile-list.txt, one hex address per line.
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.util.*;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String home = System.getenv("HOME");
        File list = new File(home + "/Code/nano-aap/build/decompile-list.txt");
        String outName = System.getenv().getOrDefault("OUT_C", "decompiled.c");
        File out = new File(home + "/Code/nano-aap/build/" + outName);
        DecompInterface dec = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        dec.setOptions(opts);
        dec.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        int ok = 0, bad = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(list));
             PrintWriter w = new PrintWriter(out)) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                Address a = toAddr(Long.parseLong(line.replace("0x", ""), 16));
                Function f = fm.getFunctionContaining(a);
                if (f == null) { w.printf("// %s : no function%n%n", line); bad++; continue; }
                DecompileResults res = dec.decompileFunction(f, 60, monitor);
                w.printf("// ===== %s  %s%n", f.getEntryPoint(), f.getName());
                if (res != null && res.decompileCompleted()) {
                    w.println(res.getDecompiledFunction().getC());
                    ok++;
                } else {
                    w.println("// decompile failed");
                    bad++;
                }
                w.println();
            }
        }
        println("DecompileAt: " + ok + " ok, " + bad + " failed -> " + out);
    }
}
