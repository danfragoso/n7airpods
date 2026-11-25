// RefsToRange.java — every reference whose TARGET is inside a data range of interest.
// Args via env: RANGE_LO / RANGE_HI (hex). Prints src -> target, plus containing function.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

import java.io.*;

public class RefsToRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        long lo = Long.decode(System.getenv().getOrDefault("RANGE_LO", "0x0890b000"));
        long hi = Long.decode(System.getenv().getOrDefault("RANGE_HI", "0x0890d000"));
        String home = System.getenv("HOME");
        File out = new File(home + "/Code/nano-aap/build/refs-to-range.txt");
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();
        int n = 0;
        try (PrintWriter w = new PrintWriter(out)) {
            for (MemoryBlock b : mem.getBlocks()) {
                if (!b.isInitialized()) continue;
                for (Address a = b.getStart(); a.compareTo(b.getEnd()) < 0; a = a.add(4)) {
                    long v = a.getOffset();
                    if (v < lo || v >= hi) continue;
                    ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
                    while (it.hasNext()) {
                        Reference r = it.next();
                        Function f = fm.getFunctionContaining(r.getFromAddress());
                        w.printf("%08x -> %08x  type=%s fn=%s%n", r.getFromAddress().getOffset(), v,
                                 r.getReferenceType(), f == null ? "-" : f.getEntryPoint().toString());
                        n++;
                    }
                }
            }
        }
        println("RefsToRange: " + n + " refs -> " + out);
    }
}
