#!/usr/bin/env python3
"""rd.py ADDR [N] — read live RAM (real RAM addresses) and print as u32/u16 words."""
import subprocess, sys, os, tempfile, struct
SG = os.environ.get('IPOD_SG_RAW', os.path.join(os.environ.get('NANOAPPS', os.path.expanduser('~/NanoApps')), 'tools/macos/ipod_sg_raw'))
def read(addr, n):
    out = bytearray()
    while len(out) < n:
        a = addr + len(out); take = min(512, n - len(out))
        for _ in range(8):
            fd, p = tempfile.mkstemp(); os.close(fd)
            subprocess.run([SG, '-o', p, '-r', '512', 'auto', 'c6', '98', '02',
                            f'{(a>>24)&255:02x}', f'{(a>>16)&255:02x}', f'{(a>>8)&255:02x}', f'{a&255:02x}'],
                           capture_output=True)
            data = open(p,'rb').read(); os.unlink(p)
            if data and data.count(0) != len(data): break
        out += (data + b'\0'*512)[:take]
    return bytes(out)
addr = int(sys.argv[1], 0); n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 32
d = read(addr, n)
for i in range(0, len(d), 4):
    w, = struct.unpack_from('<I', d, i)
    print(f"  {addr+i:#010x}: {w:#010x}  {d[i:i+4].hex(' ')}")
