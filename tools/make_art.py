#!/usr/bin/env python3
"""make_art.py — turn the extracted Apple AirPods renders into device-ready LVGL images.

    python3 tools/make_art.py [ASSET_DIR]      (or set AIRPODS_ASSETS)
    -> app/aap_ctl/aap_art.c, app/aap_ctl/aap_art.h

Why the odd format: the nano's LVGL is built without ARGB8888 image support (sdk/lv_conf.h), but it does
draw RGB565 sources. The page background behind the pictures is pure black, so each render is composited
onto black (premultiplied, which also gives clean anti-aliased edges), converted to the sRGB colour space,
scaled to the on-screen size, error-diffused to RGB565 (no banding in the soft gradients) and embedded as
a C array. About 20 KB per model.

The charging-case renders have a green LED baked in. We want to draw the LED in the state the case is
really in, so the LED pixels are found, painted over with the surrounding case colour, and their position
is exported (led_x, led_y, led_r)."""
import os, sys, io
from PIL import Image, ImageCms

ASSETS = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("AIRPODS_ASSETS", "")
if not os.path.isdir(ASSETS):
    sys.exit("usage: %s ASSET_DIR   (or set AIRPODS_ASSETS) - a folder of AirPods renders, see docs/ASSETS.md" % sys.argv[0])
FORMAT = os.environ.get("ART_FORMAT", "rgb565")      # rgb565 (default) | xrgb (4 bytes/pixel, cheaper to draw)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "app", "aap_ctl")

CASE_W, CASE_H = 68, 60        # the closed case fits 68 x 60 (a column is 72 wide, the slot 64 high)
MAX_W, MAX_H = 70, 64          # AirPods Max fits 70 x 64
# The buds use the SAME scale as their case: Apple's renders of one generation share a pixel scale, so
# the relative sizes are the real ones (a Pro bud is about half as tall as its case is wide).

# id, symbol, folder, left, right, case (right None = a single picture)
#
# The left render goes in the left slot and the right render in the right slot, as Apple draws them: the
# orientation was checked against Apple's own reference material and is correct for every model. (Do not
# swap them: that was tried and reverted.) How close the two buds sit on the page is decided by the page
# (BUD_PULL in aap_ui.c), not here.
SETS = [
    (1, "airpods_1",     "airpods-1",         "left@3x", "right@3x", "case-closed@3x"),
    (2, "airpods_2",     "airpods-2",         "left@3x", "right@3x", "case-closed@3x"),
    (3, "airpods_3",     "airpods-3",         "left@3x", "right@3x", "case-closed@3x"),
    (4, "airpods_4",     "airpods-4",         "left@3x", "right@3x", "case-closed@3x"),
    (5, "airpods_pro_1", "airpods-pro-1",     "left@3x", "right@3x", "case-closed@3x"),
    (6, "airpods_pro_2", "airpods-pro-2",     "left@3x", "right@3x", "case-closed@3x"),
    (7, "airpods_pro_3", "airpods-pro-3",     "left-small@2x", "right-small@2x", "case-closed-charged@2x"),
    (8, "airpods_max",   "airpods-max",       "headphones-space-gray@3x", None, None),
    (9, "airpods_max_c", "airpods-max-usb-c", "headphones-midnight@3x",   None, None),
]

SRGB = ImageCms.createProfile("sRGB")


def load(folder, name):
    im = Image.open(os.path.join(ASSETS, folder, name + ".png"))
    icc = im.info.get("icc_profile")
    im = im.convert("RGBA")
    if icc:                                           # the renders carry a wide-gamut profile
        try:
            src = ImageCms.ImageCmsProfile(io.BytesIO(icc))
            rgb = ImageCms.profileToProfile(im.convert("RGB"), src, SRGB, outputMode="RGB")
            im = Image.merge("RGBA", (*rgb.split(), im.getchannel("A")))
        except Exception as e:
            print("  (no colour conversion: %s)" % e)
    return im


def find_led(im):
    """Pixels of the baked-in green LED: strongly green, opaque."""
    px = im.load()
    pts = []
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a > 200 and g > 110 and g > r * 1.3 and g > b * 1.3:
                pts.append((x, y))
    return pts


def erase_led(im, pts):
    """Paint the LED away with the average of the opaque, non-LED pixels around it."""
    if not pts:
        return im
    px = im.load()
    led = set(pts)
    grown = set()
    for (x, y) in pts:
        for dy in range(-3, 4):
            for dx in range(-3, 4):
                grown.add((x + dx, y + dy))
    out = im.copy()
    po = out.load()
    for (x, y) in grown:
        if not (0 <= x < im.width and 0 <= y < im.height):
            continue
        sr = sg = sb = n = 0
        for dy in range(-9, 10):
            for dx in range(-9, 10):
                xx, yy = x + dx, y + dy
                if (xx, yy) in grown or not (0 <= xx < im.width and 0 <= yy < im.height):
                    continue
                r, g, b, a = px[xx, yy]
                if a > 250:
                    sr += r; sg += g; sb += b; n += 1
        if n:
            po[x, y] = (sr // n, sg // n, sb // n, 255)
    return out


def fit(im, bw, bh):
    s = min(bw / im.width, bh / im.height)
    return max(1, round(im.width * s)), max(1, round(im.height * s))


def to_black(im, size):
    """Premultiply onto black, then resize (premultiplied resizing has no dark/bright fringes)."""
    r, g, b, a = im.split()
    from PIL import ImageChops
    pre = Image.merge("RGB", (ImageChops.multiply(r, a), ImageChops.multiply(g, a), ImageChops.multiply(b, a)))
    return pre.resize(size, Image.LANCZOS)


def rgb565(im):
    """Floyd-Steinberg to RGB565, little-endian bytes."""
    w, h = im.size
    src = [[list(map(float, im.getpixel((x, y)))) for x in range(w)] for y in range(h)]
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = src[y][x]
            q = []
            for v, bits in ((r, 5), (g, 6), (b, 5)):
                m = (1 << bits) - 1
                c = min(m, max(0, int(round(v * m / 255.0))))
                q.append(c)
                err = v - c * 255.0 / m
                # distribute the error of this channel
                ch = len(q) - 1
                for dx, dy, wt in ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < w and yy < h:
                        src[yy][xx][ch] += err * wt / 16.0
            v = (q[0] << 11) | (q[1] << 5) | q[2]
            out += bytes((v & 0xFF, v >> 8))
    return bytes(out)


def xrgb8888(im):
    out = bytearray()
    for (r, g, b) in im.getdata():
        out += bytes((b, g, r, 0xFF))
    return bytes(out)


def pack(im):
    return xrgb8888(im) if FORMAT == "xrgb" else rgb565(im)


def emit_array(f, name, data):
    f.write("static const uint8_t %s_px[] __attribute__((aligned(4))) = {\n" % name)
    for i in range(0, len(data), 24):
        f.write("    " + ",".join("0x%02x" % c for c in data[i:i + 24]) + ",\n")
    f.write("};\n")
    return len(data)


def main():
    CF = "LV_COLOR_FORMAT_XRGB8888" if FORMAT == "xrgb" else "LV_COLOR_FORMAT_RGB565"
    img = {}        # symbol -> (w, h, bytes)
    meta = {}       # id -> dict
    total = 0
    for (aid, sym, folder, l, r, c) in SETS:
        m = {"sym": sym, "left": None, "right": None, "case": None, "led": (-1, -1, 0)}
        if r is None:                                              # a single picture
            im = load(folder, l)
            size = fit(im, MAX_W, MAX_H)
            small = to_black(im, size)
            img[sym + "_one"] = (size[0], size[1], pack(small)); m["left"] = sym + "_one"
        else:
            case_im = load(folder, c)
            scale = min(CASE_W / case_im.width, CASE_H / case_im.height)      # one scale for all three
            for key, nm in (("left", l), ("right", r), ("case", c)):
                im = case_im if key == "case" else load(folder, nm)
                size = (max(1, round(im.width * scale)), max(1, round(im.height * scale)))
                if key == "case":
                    pts = find_led(im)
                    if pts:
                        cx = sum(p[0] for p in pts) / len(pts); cy = sum(p[1] for p in pts) / len(pts)
                        im = erase_led(im, pts)
                        sx = size[0] / im.width; sy = size[1] / im.height
                        rad = (len(pts) / 3.14159) ** 0.5 * sx
                        m["led"] = (round(cx * sx), round(cy * sy), max(1, round(rad)))
                small = to_black(im, size)
                img["%s_%s" % (sym, key)] = (size[0], size[1], pack(small)); m[key] = "%s_%s" % (sym, key)
        meta[aid] = m
        print("%-14s %s  led=%s" % (sym, [(k, img[m[k]][:2]) for k in ("left", "right", "case") if m[k]], m["led"]))

    with open(os.path.join(OUT, "aap_art.h"), "w") as f:
        f.write("""/* aap_art.h — GENERATED by tools/make_art.py from Apple's AirPods renders. Do not edit.
 *
 * RGB565 pictures, pre-composited on black (the page background). One set per generation. */
#ifndef AAP_ART_H
#define AAP_ART_H

#include "lvgl/lvgl.h"

typedef struct {
    const lv_image_dsc_t *left, *right, *cas;   /* right == cas == NULL: a single picture (AirPods Max) */
    int8_t led_x, led_y, led_r;                 /* where the case LED sits in `cas`; led_x < 0: none     */
} aap_art_t;

/* art ids as used by the model table; 0 = no pictures (the page draws a generic pair itself) */
const aap_art_t *aap_art_get(unsigned id);

#endif
""")
    with open(os.path.join(OUT, "aap_art.c"), "w") as f:
        f.write("/* aap_art.c — GENERATED by tools/make_art.py. Do not edit. */\n#include \"aap_art.h\"\n\n")
        for name, (w, h, data) in img.items():
            total += emit_array(f, name, data)
            f.write("static const lv_image_dsc_t %s = {\n"
                    "    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = %s, .w = %d, .h = %d, .stride = %d },\n"
                    "    .data_size = %d, .data = %s_px };\n\n" % (name, CF, w, h, w * (4 if FORMAT == "xrgb" else 2), len(data), name))
        f.write("static const aap_art_t k_art[] = {\n    { 0 },\n")
        for aid in sorted(meta):
            m = meta[aid]
            one = m["right"] is None
            f.write("    { %s, %s, %s, %d, %d, %d },   /* %d %s */\n" % (
                "&" + m["left"], "0" if one else "&" + m["right"], "0" if one else "&" + m["case"],
                m["led"][0], m["led"][1], m["led"][2], aid, m["sym"]))
        f.write("};\n\nconst aap_art_t *aap_art_get(unsigned id)\n{\n"
                "    return (id > 0 && id < sizeof k_art / sizeof k_art[0]) ? &k_art[id] : 0;\n}\n")
    print("pixel data: %d bytes" % total)


main()
