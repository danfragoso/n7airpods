#!/usr/bin/env python3
"""make_icon.py — the home-screen icon art for the AirPods app: ONE real AirPods Pro bud (Apple's own render),
tilted so it fills the round icon the way the stock glyphs do, with a soft drop shadow.

    python3 tools/make_icon.py [ASSET_DIR]   ->  app/aap_ctl/icon_art.png

tools/build_apps.py (NanoApps) fits apps/<name>/icon_art.png inside its glossy colour disc, over the gloss,
keeping the art's own colours. History: the flat SF Symbol `airpods.pro.left` (white silhouette) looked
lopsided and clunky next to the stock icons; the two-pod symbol looked tall and stretched. A real, upright
bud looked small and stiff in the disc; tilted ~32 degrees it uses the disc's diagonal and reads at once.
The disc colour is HBIconColor in Info.plist (#34c759, a vivid green no other app uses)."""
import os, sys
from PIL import Image, ImageFilter, ImageChops

ASSETS = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("AIRPODS_ASSETS", "")
if not os.path.isdir(ASSETS):
    sys.exit("usage: %s ASSET_DIR   (or set AIRPODS_ASSETS) - a folder of AirPods renders, see docs/ASSETS.md" % sys.argv[0])
SRC = os.path.join(ASSETS, "airpods-pro-2", "left@3x.png")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "app", "aap_ctl", "icon_art.png")
TILT = 32            # degrees counter-clockwise
HEIGHT = 560         # working size; build_apps scales it down (4x supersampled) so edges stay smooth

im = Image.open(SRC).convert("RGBA")
im = im.crop(im.getchannel("A").getbbox())
s = HEIGHT / im.height
im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)

pad = Image.new("RGBA", (im.width * 2, im.height * 2), (0, 0, 0, 0))     # rotate with room to spare
pad.alpha_composite(im, (im.width // 2, im.height // 2))
pad = pad.rotate(TILT, Image.BICUBIC)
im = pad.crop(pad.getchannel("A").getbbox())

M = 10                                                                   # room for the shadow
W, H = im.width + 2 * M, im.height + 2 * M
a = Image.new("L", (W, H), 0)
a.paste(im.getchannel("A"), (M, M))
sh = ImageChops.offset(a.filter(ImageFilter.GaussianBlur(4)), 0, 5).point(lambda v: int(v * 0.35))
out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
layer.putalpha(sh)
out = Image.alpha_composite(out, layer)
out.alpha_composite(im, (M, M))
out.save(OUT)
print("wrote", os.path.normpath(OUT), out.size)
