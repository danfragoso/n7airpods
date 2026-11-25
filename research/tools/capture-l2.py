#!/usr/bin/env python3
"""capture-l2.py — snapshot the live L2CAP tables from the nano over SCSI.

Reads:
  * the L2CAP link registry pointer   (global at 0x084e9ab0)
  * the 24-slot channel table         (state struct at 0x0895c080 +4 -> CCB array)

Each CCB is 0x88 bytes. Field map derived from FUN_080f21c0 / FUN_080f2274 /
FUN_080f7da0 (see docs/FINDINGS.md):
  +0x00 u8  state      0 free, 1 ?, 2 open, 3 allocating
  +0x02 u16 id         caller-supplied profile id (5 and 6 observed)
  +0x04 u16 cid
  +0x24 ..  small fields (+0x24 next ptr, +0x25.. order)
  +0x28 u16 mtu        (672 default from the allocator, 0x17 seen on AVRCP)
  +0x2a u16 flush
  +0x2e .. 6-byte remote BD_ADDR
  +0x34 u8  ...
  +0x34..+0x39 state flags
  +0x3c u16 callback id passed to the allocator
  +0x40..+0x48 mtu/flush/latency/timeouts
"""
import subprocess, os, tempfile, struct, sys

SG = os.environ.get('IPOD_SG_RAW', os.path.join(os.environ.get('NANOAPPS', os.path.expanduser('~/NanoApps')), 'tools/macos/ipod_sg_raw'))

def read(addr, n):
    out = bytearray()
    while len(out) < n:
        a = addr + len(out); take = min(512, n - len(out))
        for _ in range(8):
            fd, p = tempfile.mkstemp(); os.close(fd)
            subprocess.run([SG, '-o', p, '-r', '512', 'auto', 'c6', '98', '02',
                            f'{(a>>24)&255:02x}', f'{(a>>16)&255:02x}',
                            f'{(a>>8)&255:02x}', f'{a&255:02x}'], capture_output=True)
            d = open(p, 'rb').read(); os.unlink(p)
            if d and d.count(0) != len(d): break
        out += (d + b'\0'*512)[:take]
    return bytes(out)

def u16(b,o): return struct.unpack_from('<H', b, o)[0]
def u32(b,o): return struct.unpack_from('<I', b, o)[0]

REG_PTR   = 0x084e9ab0      # global: pointer to count*8 link registry
STATE_PTR = 0x0895c080      # L2CAP channel state (count at +2, CCB array ptr at +4)

print("=== link registry pointer slot")
reg = u32(read(REG_PTR, 4), 0)
print(f"  [{REG_PTR:#010x}] -> {reg:#010x}")
if 0x08000000 <= reg < 0x09800000:
    blob = read(reg, 10*8)
    for i in range(10):
        a, b = struct.unpack_from('<II', blob, i*8)
        if a or b: print(f"    link{i}: {a:#010x} {b:#010x}")

print("\n=== channel state")
st = read(STATE_PTR, 16)
n = u16(st, 2); arr = u32(st, 4)
print(f"  state={u16(st,0)} count={n} ccb_array={arr:#010x}")
if not (0x08000000 <= arr < 0x09800000):
    print("  (ccb array pointer looks invalid — stack may not be initialised)")
    sys.exit(0)

SZ = 0x88
d = read(arr, n*SZ)
live = 0
print(f"\n  {'ch':>3} {'st':>2} {'id':>4} {'cid':>5} {'mtu':>6} {'flush':>6} {'bd_addr':>18} {'+34':>4} {'+3c':>4}")
for ch in range(n):
    o = ch*SZ
    state = d[o]
    if state == 0: continue
    live += 1
    bd = ':'.join(f'{d[o+0x2e+i]:02x}' for i in range(6))
    print(f"  {ch:3d} {state:2d} {u16(d,o+2):4d} {u16(d,o+4):5d} {u16(d,o+0x28):6d} "
          f"{u16(d,o+0x2a):#6x} {bd:>18} {d[o+0x34]:4d} {u16(d,o+0x3c):#4x}")
print(f"\n  {live} live channel(s) of {n}")
