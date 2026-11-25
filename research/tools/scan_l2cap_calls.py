#!/usr/bin/env python3
"""Find calls into the L2CAP module and show the constants set up before them.

We do not have a decompiler on this Mac (Ghidra 12.2 ships x86 natives only), so:
disassemble every function Ghidra found with capstone (Thumb), and print the window
preceding each `bl` whose target lands in the L2CAP module range. PSMs we care about:
0x0001 SDP, 0x0003 RFCOMM, 0x0017 AVRCP, 0x0019 AVDTP, and 0x1001 (AAP, ours).
"""
import re, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN

BASE = 0x08000000
LO, HI = (int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x080c8000,
          int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x080d4000)
import os
ROOT  = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FUNCS = os.path.join(ROOT, 'build', 'functions.txt')
BIN   = os.path.join(ROOT, 'build', 'osos.bin')

d = open(BIN, 'rb').read()
fns = []
for line in open(FUNCS):
    a, n = line.split()
    fns.append(int(a, 16))
fns.sort()

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
md.detail = True

def dis(addr, size):
    off = addr - BASE
    return list(md.disasm(d[off:off+size], addr))

hits = 0
for i, f in enumerate(fns):
    end = fns[i+1] if i+1 < len(fns) else BASE + len(d)
    size = min(end - f, 0x2000)
    ins = dis(f, size)
    for j, insn in enumerate(ins):
        if insn.mnemonic not in ('bl', 'blx'):
            continue
        try:
            tgt = int(insn.op_str, 16) & ~1
        except ValueError:
            continue
        if not (LO <= tgt < HI):
            continue
        win = ins[max(0, j-6):j+1]
        imm = ' '.join(x.op_str for x in win)
        flag = ''
        if re.search(r'#0x1001|#0x19\b|#0x1a\b|#0x17\b|#25\b|#23\b|#4097', imm):
            flag = '  <<<< PSM-ish'
        print(f"caller {f:#010x} -> {tgt:#010x}  [{ ' ; '.join(x.mnemonic+' '+x.op_str for x in win) }]{flag}")
        hits += 1
print(f"\n{hits} calls into {LO:#x}-{HI:#x}")
