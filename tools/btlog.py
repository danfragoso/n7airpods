#!/usr/bin/env python3
"""btlog.py — read the iPod nano's Bluetooth flight recorder over USB and print it as a log.

The resident (integration/btlog.c) taps the firmware's BT logger method and keeps every call in a
ring in DRAM. This reads that ring with `start peek` — no reboot, no disk mode, works from the
normal home screen — and resolves each record's format string against the decrypted firmware image.

  btlog.py                     last 200 records
  btlog.py -n 1000 -g isconn   last 1000 records containing "isconn"
  btlog.py --since 90000       only records at/after t=90000 ms (device uptime)
  btlog.py --save run1.bin     also keep the raw ring (decode later with --file)
  btlog.py --file run1.bin     decode a saved ring instead of reading the device
  btlog.py --selftest          decode a synthetic ring built from real firmware strings (no device)

Address model (firmware 1.1.2 / 39A10023): live RAM = 0x08000000 + file_offset - 0x400.
"""
import argparse, os, re, struct, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_IMAGE = HERE.parent / "build" / "osos.bin"
DEFAULT_START = Path(os.environ.get("NANOAPPS", Path.home() / "NanoApps")) / "start"
BOOT_REPORT = 0x086c9b84
REPORT_MAGIC_OFF, REPORT_MAGIC = 0x2c, 0x32505452
RING_PTR_OFF = 0x14c
BTL_MAGIC = 0x4c544254
HDR = 32

class Image:
    def __init__(self, path):
        self.d = Path(path).read_bytes()
    def off(self, ram):
        o = ram - 0x08000000 + 0x400
        return o if 0 <= o < len(self.d) else None
    def cstr(self, ram, limit=400):
        o = self.off(ram)
        if o is None: return None
        e = self.d.find(b"\0", o, o + limit)
        if e < 0: return None
        s = self.d[o:e]
        if not s or any(b < 9 or (13 < b < 32) or b > 126 for b in s): return None
        return s.decode("ascii")

SPEC = re.compile(r"%([-+ #0]*)(\*|\d+)?(?:\.(\*|\d+))?(hh|h|ll|l|z|t|j|L)?([diuoxXcspfeEgG%])")

def render(fmt, args, cap_str, img):
    """printf-style rendering with 4 raw words. %s uses the string captured at log time for the
    first %s, a firmware-image string if the pointer is static, else the raw pointer."""
    out, ai, used_cap = [], 0, False
    pos = 0
    for m in SPEC.finditer(fmt):
        out.append(fmt[pos:m.start()]); pos = m.end()
        flags, width, prec, ln, conv = m.groups()
        if conv == "%":
            out.append("%"); continue
        if ai >= 4:
            out.append("?"); continue
        v = args[ai]; ai += 1
        if ln == "ll": ai += 1
        w = int(width) if width and width != "*" else 0
        try:
            if conv in "di":
                n = v - (1 << 32) if v & 0x80000000 else v
                t = str(n)
            elif conv == "u": t = str(v)
            elif conv in "xX":
                t = format(v, "x" if conv == "x" else "X")
                if "#" in (flags or ""): t = "0x" + t
            elif conv == "o": t = format(v, "o")
            elif conv == "c": t = chr(v & 0xff) if 32 <= (v & 0xff) < 127 else "."
            elif conv == "p": t = f"0x{v:08x}"
            elif conv == "s":
                if not used_cap and cap_str:
                    t = cap_str; used_cap = True
                else:
                    s = img.cstr(v)
                    t = s if s is not None else f"<0x{v:08x}>"
            else: t = f"<{conv}:0x{v:08x}>"
        except Exception:
            t = f"<0x{v:08x}>"
        if "0" in (flags or "") and conv not in "sc" and w: t = t.zfill(w)
        elif w: t = t.ljust(w) if "-" in (flags or "") else t.rjust(w)
        out.append(t)
    out.append(fmt[pos:])
    return "".join(out)

MODELS = {0x0e20: "AirPods Pro", 0x1420: "AirPods Pro 2", 0x2420: "AirPods Pro 2 (USB-C)",
          0x0220: "AirPods 1", 0x0f20: "AirPods 2", 0x1320: "AirPods 3", 0x1920: "AirPods 4",
          0x1b20: "AirPods 4 (ANC)", 0x0a20: "AirPods Max", 0x1f20: "AirPods Max (USB-C)"}

def _nib(n):
    return "?" if n == 15 else f"{100 if n >= 10 else n * 10}%"

def apple_payload(data):
    """Manufacturer payload after the 0x004C company id, from raw advertising bytes (AD structures
    when they parse, else the first 4C 00 found)."""
    i = 0
    while i + 1 < len(data):
        ln = data[i]
        if ln == 0: break
        if data[i + 1] == 0xff and data[i + 2:i + 4] == b"\x4c\x00":
            return data[i + 4:i + 1 + ln]
        i += 1 + ln
    j = data.find(b"\x4c\x00")
    return data[j + 2:] if j >= 0 else None

def decode_apple(p):
    """LibrePods' proximity-pairing layout (type 0x07, 25 bytes); plain fields only."""
    if p[0] != 0x07 or len(p) < 11:
        return f"Apple type 0x{p[0]:02x} " + p[:12].hex(" ")
    paired, model, st, pods, fl, lid, color, conn = p[2], (p[3] << 8) | p[4], p[5], p[6], p[7], p[8], p[9], p[10]
    prim_left = bool(st & 0x20); this_in_case = bool(st & 0x40); flip = not prim_left
    ln = (pods >> 4) & 15 if flip else pods & 15
    rn = pods & 15 if flip else (pods >> 4) & 15
    cn, flags = fl & 15, (fl >> 4) & 15
    lc = bool(flags & (2 if flip else 1)); rc = bool(flags & (1 if flip else 2)); cc = bool(flags & 4)
    x = flip ^ this_in_case
    l_ear = bool(st & (8 if x else 2)); r_ear = bool(st & (2 if x else 8))
    lid_open = ((lid >> 3) & 1) == 0
    return (f"AIRPODS {MODELS.get(model, hex(model))} paired={paired} L={_nib(ln)}{'+' if lc else ''} "
            f"R={_nib(rn)}{'+' if rc else ''} CASE={_nib(cn)}{'+' if cc else ''} lid={'open' if lid_open else 'closed'}"
            f"#{lid & 7} inEar L{int(l_ear)} R{int(r_ear)} thisPodInCase={int(this_in_case)} "
            f"oneInCase={int(bool(st & 0x10))} bothInCase={int(bool(st & 4))} conn={conn} status=0x{st:02x}")

def records(buf):
    magic, wr, cap, recb, sampled, ver, hooked, letap = struct.unpack_from("<8I", buf, 0)
    if magic != BTL_MAGIC:
        raise SystemExit(f"ring magic {magic:08x} is not 'TBTL' — recorder not installed this boot?")
    n = min(wr, cap)
    first = wr - n
    recs = []
    for seq in range(first, wr):
        o = HDR + (seq % cap) * recb
        w = struct.unpack_from("<12I", buf, o)
        if w[0] == 0 and w[2] == 0: continue          # never written / torn
        recs.append((seq, w))
    return dict(wr=wr, cap=cap, recb=recb, sampled=sampled, ver=ver, hooked=hooked, letap=letap), recs

def show(buf, img, last, grep, since):
    info, recs = records(buf)
    print(f"# btlog v{info['ver']}  records written={info['wr']}  ring={info['cap']}  "
          f"flood-skipped={info['sampled']}  hooked={info['hooked']}  le-tap={info['letap']}")
    rx = re.compile(grep, re.I) if grep else None
    rows = []
    for seq, w in recs:
        t, w1, fmt, lr = w[0], w[1], w[2], w[3]
        if t < since: continue
        mod, lvl, line = w1 >> 24, (w1 >> 16) & 0xff, w1 & 0xffff
        cap_str = bytes(struct.pack("<4I", *w[8:12])).split(b"\0")[0].decode("ascii", "replace")
        if (fmt & 0xffffff00) == 0xffffff00 and fmt != 0xffffffff:   # raw LE advertisement (btl_raw_adv)
            ln, typ, rssi = w1 & 0xffff, (w1 >> 16) & 0xff, fmt & 0xff
            rssi = rssi - 256 if rssi > 127 else rssi
            raw = struct.pack("<8I", *w[4:12])[:min(ln, 32)]
            ap = apple_payload(raw)
            body = decode_apple(ap) if ap else "raw " + raw[:24].hex(" ")
            msg = f"LE dev@{lr:08x} t{typ} {rssi:4d}dBm len={ln:<3d} {body}"
            lr = 0
        elif fmt == 0xffffffff:                      # a mark from our own code (btl_mark)
            msg = f"** {cap_str:<8} a=0x{w[4]:x} b=0x{w[5]:x}"
            lr = 0
        else:
            f = img.cstr(fmt)
            msg = render(f, w[4:8], cap_str, img) if f else f"<fmt 0x{fmt:08x}> args={w[4]:08x},{w[5]:08x},{w[6]:08x},{w[7]:08x}"
        line_txt = f"{t/1000:10.3f}s  L{lvl} :{line:<4} @{lr & ~1:08x}  {msg.rstrip()}"
        if rx and not rx.search(line_txt): continue
        rows.append(line_txt)
    for r in rows[-last:]:
        print(r)

def peek(start, addr, n):
    r = subprocess.run([sys.executable, str(start), "peek", hex(addr), hex(n)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"start peek failed: {r.stderr.strip() or r.stdout.strip()}\n"
                         "is the iPod attached? (any mode works)")
    data = bytearray()
    for ln in r.stdout.splitlines():
        m = re.match(r"^([0-9a-f]{8})  ((?:[0-9a-f]{2} ?){1,16})", ln)
        if m: data += bytes.fromhex(m.group(2).replace(" ", ""))
    if len(data) < n:
        raise SystemExit(f"short peek: wanted {n:#x} bytes, got {len(data):#x}")
    return bytes(data[:n])

def read_device(start):
    rep = peek(start, BOOT_REPORT, 0x200)
    if struct.unpack_from("<I", rep, REPORT_MAGIC_OFF)[0] != REPORT_MAGIC:
        raise SystemExit("no boot report: the resident has not run this boot")
    ring = struct.unpack_from("<I", rep, RING_PTR_OFF)[0]
    if not 0x08d00000 <= ring < 0x0a000000:
        raise SystemExit(f"resident published ring={ring:#x}: recorder not installed "
                         "(btlog.off present, slot mismatch, or an older resident)")
    h = peek(start, ring, HDR)
    magic, wr, cap, recb = struct.unpack_from("<4I", h, 0)
    if magic != BTL_MAGIC: raise SystemExit(f"bad ring magic {magic:08x} at {ring:#x}")
    return peek(start, ring, HDR + cap * recb)

def selftest(img):
    """Build a ring from real firmware format strings, wrap it, decode it. No device needed."""
    cases = [  # (corpus label address from decompiled-all.c, args, captured %s)
        (0x084361e4, [1, 0, 0, 0], "Pods-L"),    # Disconnection reported for device %s
        (0x082be0ac, [0, 0, 0, 0], "iPod"),      # Local device is %s
    ]
    cap = 4
    buf = bytearray(HDR + cap * 48)
    n = 0
    for i, (corp, args, s) in enumerate(cases * 3):         # 6 records into 4 slots -> wraps
        fmt = corp - 0x400                                   # corpus label -> live RAM pointer
        sb = s.encode().ljust(16, b"\0")
        w = [1000 + 250 * i, (0x10 << 24) | (5 << 16) | (0x22d + i), fmt, 0x08435d39 + i, *args, *struct.unpack("<4I", sb)]
        struct.pack_into("<12I", buf, HDR + (n % cap) * 48, *w); n += 1
    struct.pack_into("<8I", buf, 0, BTL_MAGIC, n, cap, 48, 0, 2, 1, 0)
    show(bytes(buf), img, 50, None, 0)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", type=int, default=200, help="show the last N matching records")
    ap.add_argument("-g", "--grep", help="regex filter on the rendered line")
    ap.add_argument("--since", type=int, default=0, help="only records at/after this uptime (ms)")
    ap.add_argument("--image", default=str(DEFAULT_IMAGE), help="decrypted firmware (osos.bin)")
    ap.add_argument("--start", default=str(DEFAULT_START), help="NanoApps `start` script")
    ap.add_argument("--save", help="write the raw ring to this file")
    ap.add_argument("--file", help="decode a saved ring instead of reading the device")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    img = Image(a.image)
    if a.selftest: return selftest(img)
    buf = Path(a.file).read_bytes() if a.file else read_device(a.start)
    if a.save: Path(a.save).write_bytes(buf)
    show(buf, img, a.n, a.grep, a.since)

if __name__ == "__main__":
    main()
