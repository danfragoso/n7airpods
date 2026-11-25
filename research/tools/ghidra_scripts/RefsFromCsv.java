// RefsFromCsv.java — for each address in build/bt-strings.csv, show code references,
// the containing function, and that function's callers.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.util.*;

public class RefsFromCsv extends GhidraScript {
    @Override
    public void run() throws Exception {
        String home = System.getenv("HOME");
        File in = new File(home + "/Code/nano-aap/build/bt-strings.csv");
        File out = new File(home + "/Code/nano-aap/build/bt-refs.txt");
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        try (BufferedReader r = new BufferedReader(new FileReader(in));
             PrintWriter w = new PrintWriter(out)) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] parts = line.split("\t", 2);
                if (parts.length < 2) continue;
                Address a = toAddr(Long.parseLong(parts[0], 16));
                w.printf("%s | %s%n", parts[0], parts[1]);
                ReferenceIterator it = rm.getReferencesTo(a);
                int n = 0;
                while (it.hasNext() && n < 12) {
                    Reference ref = it.next();
                    Address src = ref.getFromAddress();
                    Function f = fm.getFunctionContaining(src);
                    w.printf("    ref %s type=%s", src, ref.getReferenceType());
                    if (f != null) {
                        w.printf(" fn=%s", f.getEntryPoint());
                        StringBuilder cb = new StringBuilder();
                        int c = 0;
                        for (Function cf : f.getCallingFunctions(monitor)) {
                            cb.append(' ').append(cf.getEntryPoint());
                            if (++c >= 4) break;
                        }
                        w.printf(" callers:%s", cb);
                    }
                    w.println();
                    n++;
                }
                if (n == 0) w.println("    (no references)");
            }
        }
        println("RefsFromCsv done -> " + out);
    }
}
