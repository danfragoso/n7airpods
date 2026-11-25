// CallGraph.java — dump every caller->callee edge Ghidra resolved (mode-aware, unlike a
// naive Thumb-only scan). Lets us find who calls into the L2CAP module.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;

import java.io.*;

public class CallGraph extends GhidraScript {
    @Override
    public void run() throws Exception {
        String home = System.getenv("HOME");
        File out = new File(home + "/Code/nano-aap/build/callgraph.txt");
        FunctionManager fm = currentProgram.getFunctionManager();
        int n = 0;
        try (PrintWriter w = new PrintWriter(out)) {
            for (Function f : fm.getFunctions(true)) {
                for (Function c : f.getCalledFunctions(monitor)) {
                    w.printf("%08x %08x%n", f.getEntryPoint().getOffset(), c.getEntryPoint().getOffset());
                    n++;
                }
            }
        }
        println("CallGraph: " + n + " edges -> " + out);
    }
}
