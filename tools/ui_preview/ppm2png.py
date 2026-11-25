#!/usr/bin/env python3
"""ppm2png.py in.ppm out.png [scale]  — dependency-free PNG writer (nearest-neighbour upscale)."""
import sys, zlib, struct
def main():
    src, dst = sys.argv[1], sys.argv[2]; scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    d = open(src, 'rb').read()
    parts = d.split(b'\n', 3); w, h = map(int, parts[1].split()); px = parts[3]
    rows = []
    for y in range(h):
        row = px[y*w*3:(y+1)*w*3]
        if scale > 1: row = b''.join(row[i:i+3]*scale for i in range(0, len(row), 3))
        for _ in range(scale): rows.append(b'\x00' + row)
    raw = b''.join(rows)
    def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w*scale, h*scale, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)
main()
