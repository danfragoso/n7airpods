#!/usr/bin/env python3
"""Scan a DRAM range over SCSI peek and save it locally, then look for structures of interest."""
import subprocess, sys, os, struct, tempfile
SG = os.environ.get('IPOD_SG_RAW', os.path.join(os.environ.get('NANOAPPS', os.path.expanduser('~/NanoApps')), 'tools/macos/ipod_sg_raw'))
lo = int(sys.argv[1], 0); hi = int(sys.argv[2], 0)
out = sys.argv[3]
buf = bytearray()
addr = lo
blob = bytearray()
while addr < hi:
    for _ in range(8):
        with tempfile.NamedTemporaryFile(delete=False) as tf:
            p = tf.name
        r = subprocess.run([SG, '-o', p, '-r', '512', 'auto', 'c6', '98', '02',
                            f'{(addr>>24)&0xff:02x}', f'{(addr>>16)&0xff:02x}',
                            f'{(addr>>8)&0xff:02x}', f'{addr&0xff:02x}'],
                           capture_output=True)
        data = open(p, 'rb').read() if os.path.exists(p) else b''
        os.unlink(p)
        if data and data.count(0) != len(data):
            break
    blob += (data + b'\0' * 512)[:512]
    addr += 512
open(out, 'wb').write(blob)
print(f"scanned {hex(lo)}..{hex(hi)} -> {out} ({len(blob)} bytes)")
