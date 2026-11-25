// FindBt.java — locate the Bluetooth/L2CAP code in osos by string references.
//
// Ghidra recovered no symbols (all FUN_*), but the image is full of debug strings.
// For each matching string, report: the string, where it lives, who references it,
// the containing function, and that function's callers. That gives us the L2CAP
// module without a decompiler.
//
// Run: tools/ghidra.sh build/ghidra nano-aap -process osos.bin \
//        -scriptPath tools/ghidra_scripts -postScript FindBt.java -noanalysis

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataUtilities;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.File;
import java.io.PrintWriter;
import java.util.regex.Pattern;

public class FindBt extends GhidraScript {

    private static final Pattern P = Pattern.compile(
            "(?i)(l2cap|l2ca|avdtp|avctp|avrcp|rfcomm|sdp|psm)");

    @Override
    public void run() throws Exception {
        File out = new File(System.getenv("HOME") + "/Code/nano-aap/build/bt-strings.txt");
        Memory mem = currentProgram.getMemory();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        int hits = 0, withRef = 0;

        try (PrintWriter w = new PrintWriter(out)) {
            for (MemoryBlock block : mem.getBlocks()) {
                if (!block.isInitialized()) continue;
                Address a = block.getStart();
                while (a.compareTo(block.getEnd()) < 0) {
                    Data d = listing.getDataAt(a);
                    if (d != null && d.getValue() instanceof String) {
                        String s = ((String) d.getValue()).trim();
                        if (s.length() >= 4 && P.matcher(s).find() && s.length() < 160) {
                            hits++;
                            StringBuilder sb = new StringBuilder();
                            Address from = null;
                            ghidra.program.model.symbol.ReferenceIterator refs = rm.getReferencesTo(a);
                            while (refs.hasNext()) {
                                Reference r = refs.next();
                                Address src = r.getFromAddress();
                                Function f = fm.getFunctionContaining(src);
                                if (f != null) { from = f.getEntryPoint(); break; }
                                from = src;
                            }
                            sb.append(String.format("%s | %s", a, s.replace('\n', ' ')));
                            if (from != null) {
                                withRef++;
                                Function f = fm.getFunctionContaining(from);
                                sb.append(String.format(" | ref@%s", from));
                                if (f != null) {
                                    sb.append(" | fn=").append(f.getEntryPoint());
                                    int nc = 0;
                                    StringBuilder cb = new StringBuilder();
                                    for (Function c : f.getCallingFunctions(monitor)) {
                                        cb.append(' ').append(c.getEntryPoint());
                                        if (++nc >= 6) break;
                                    }
                                    sb.append(" | callers:").append(cb);
                                }
                            } else {
                                sb.append(" | (no code ref)");
                            }
                            w.println(sb);
                            a = a.add(Math.max(1, d.getLength()));
                            continue;
                        }
                    }
                    a = a.add(4);
                }
            }
        }
        println("FindBt: " + hits + " matching strings, " + withRef + " with code refs -> " + out);
    }
}
