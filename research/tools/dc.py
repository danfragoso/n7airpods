#!/usr/bin/env python3
"""dc.py ADDR [ADDR...] — print function bodies from the decompiled corpus by address.
Addresses are the Ghidra labels (image base 0x08000000); subtract 0x400 for real RAM."""
import os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CORP = os.path.join(ROOT, 'build', 'decompiled-all.c')
txt = open(CORP, errors='ignore').read()
parts = re.split(r'^// ===== (\w+)\s+(\S+)$', txt, flags=re.M)
funcs = {int(parts[i], 16): parts[i+2] for i in range(1, len(parts), 3)}
for a in sys.argv[1:]:
    k = int(a, 0)
    if k not in funcs:
        print(f"// {a}: not found"); continue
    print(f"// ===== {k:#010x}  (RAM {k - 0x400:#010x})")
    print(funcs[k].strip())
    print()
