// DumpFunctions.java — export every function symbol Ghidra recovered from osos.
// Run: tools/ghidra.sh build/ghidra nano-aap -process osos.bin \
//        -scriptPath tools/ghidra_scripts -postScript DumpFunctions.java

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

import java.io.File;
import java.io.PrintWriter;

public class DumpFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        File out = new File(System.getenv("HOME") + "/Code/nano-aap/build/functions.txt");
        FunctionManager fm = currentProgram.getFunctionManager();
        int n = 0;
        try (PrintWriter w = new PrintWriter(out)) {
            for (Function f : fm.getFunctions(true)) {
                w.printf("%08x %s%n", f.getEntryPoint().getOffset(), f.getName());
                n++;
            }
        }
        println("DumpFunctions: wrote " + n + " functions to " + out);
    }
}
