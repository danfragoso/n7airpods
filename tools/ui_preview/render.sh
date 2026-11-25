#!/bin/sh
# render.sh - build the desktop preview of the AirPods page and render scenarios.
#
#   NANOAPPS=/path/to/NanoApps tools/ui_preview/build_host_lvgl.sh      # once
#   NANOAPPS=/path/to/NanoApps tools/ui_preview/render.sh [scenario ...]    # default: all (0..12)
#
# Output goes to build/hostlv/out/ as s<N>.png (full page, 2x) and c<N>.png (first screen, 1x).
# The page code is the code that runs on the nano; the scenarios are mock hb_aap_state_t values (preview.c).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/../.."
N="${NANOAPPS:-$ROOT/.nanoapps}"
H="${HOSTLV:-$ROOT/build/hostlv}"
[ -f "$H/liblvhost.a" ] || { echo "run tools/ui_preview/build_host_lvgl.sh first" >&2; exit 1; }
A="$ROOT/app/aap_ctl"
cc -O1 -Wall -Wno-unused-function -DAAP_UI_PREVIEW -DHB_LV_RELOC -DLV_CONF_INCLUDE_SIMPLE \
   -I "$ROOT/integration" -I "$N" -I "$N/sdk" -I "$N/lvgl" -I "$A" \
   -o "$H/preview" "$HERE/preview.c" "$A/aap_ui.c" "$A/aap_models.c" "$A/aap_art.c" "$A/aap_icons.c" \
   "$A/aap_font_14.c" "$A/aap_font_16.c" "$A/aap_font_20.c" "$A/aap_snap.c" "$H/liblvhost.a"
mkdir -p "$H/out"
for s in ${@:-0 1 2 3 4 5 6 7 8 9 10 11 12}; do
  "$H/preview" "$H/out/s$s.ppm" "$s" 432 && python3 -I "$HERE/ppm2png.py" "$H/out/s$s.ppm" "$H/out/s$s.png" 2
  "$H/preview" "$H/out/c$s.ppm" "$s" 432 230 && python3 -I "$HERE/ppm2png.py" "$H/out/c$s.ppm" "$H/out/c$s.png" 1
done
echo "rendered into $H/out"
