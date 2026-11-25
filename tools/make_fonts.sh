#!/bin/sh
# make_fonts.sh — regenerate app/aap_ctl/aap_font_{14,16,20}.c: Inter (SIL OFL 1.1) as LVGL fonts.
#   needs: node/npm and network (installs lv_font_conv and @fontsource/inter into a temp dir)
#   Inter 14 and 16 regular, Inter 20 semibold; ASCII 0x20-0x7E; 4 bpp; no compression (the device's
#   lv_conf has LV_USE_FONT_COMPRESSED 0); glyph kerning kept.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$HERE/../app/aap_ctl"
T="${TMPDIR:-/tmp}/aap-fonts.$$"; mkdir -p "$T"; cd "$T"
npm install --no-audit --no-fund lv_font_conv@1.5.3 @fontsource/inter@5.3.0 >/dev/null
F=node_modules/@fontsource/inter/files
gen() { ./node_modules/.bin/lv_font_conv --font $F/inter-latin-$2-normal.woff --size $1 --bpp 4 --range 0x20-0x7E \
          --format lvgl --lv-font-name aap_inter_$1 --lv-include lvgl/lvgl.h --no-compress -o "$OUT/aap_font_$1.c"; }
gen 14 400; gen 16 400; gen 20 600
cp node_modules/@fontsource/inter/LICENSE "$HERE/../docs/fonts/Inter-OFL.txt"
rm -rf "$T"; echo "wrote $OUT/aap_font_{14,16,20}.c"
