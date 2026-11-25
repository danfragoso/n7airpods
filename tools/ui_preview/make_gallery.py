#!/usr/bin/env python3
"""make_gallery.py - compose the README screenshots from the renders of render.sh.

    python3 tools/ui_preview/make_gallery.py [RENDER_DIR]     (default build/hostlv/out)
    -> docs/screenshots/{page,models,states}.png

These are renders of the real page code with mock data (tools/ui_preview/preview.c), not photos."""
import os, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "hostlv", "out")
OUT = os.path.join(ROOT, "docs", "screenshots")
BG = (18, 18, 20)
PAD, CAP = 28, 44


def font(size):
    for p in ("/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            pass
    return ImageFont.load_default()


def sheet(items, name):
    """items: [(png basename, caption)] laid out in one row on a dark background."""
    imgs = [(Image.open(os.path.join(SRC, f)).convert("RGB"), cap) for f, cap in items]
    w = PAD + sum(i.width + PAD for i, _ in imgs)
    h = max(i.height for i, _ in imgs) + PAD * 2 + CAP
    out = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(out)
    f = font(22)
    x = PAD
    for im, cap in imgs:
        out.paste(im, (x, PAD))
        tw = d.textlength(cap, font=f)
        d.text((x + (im.width - tw) / 2, PAD + im.height + 10), cap, fill=(200, 200, 205), font=f)
        x += im.width + PAD
    os.makedirs(OUT, exist_ok=True)
    out.save(os.path.join(OUT, name), optimize=True)
    print("wrote", os.path.join("docs", "screenshots", name), out.size)


sheet([("s0.png", "AirPods Pro 2"), ("c0.png", "scrolled")], "page.png")
sheet([("s1.png", "AirPods Pro"), ("s12.png", "AirPods 4"), ("s2.png", "AirPods 3"), ("s3.png", "AirPods Max")], "models.png")
sheet([("s4.png", "no AirPods"), ("s6.png", "connecting")], "states.png")
