#!/bin/sh
# build_host_lvgl.sh - compile the LVGL that NanoApps pins, for the desktop (macOS/Linux), into
# build/hostlv/liblvhost.a. render.sh links the page against it so the UI can be rendered without a device.
#
#   NANOAPPS=/path/to/NanoApps tools/ui_preview/build_host_lvgl.sh
#
# NANOAPPS must be a NanoApps checkout whose lvgl/ folder exists (run `./start build aap_ctl` there once,
# or ./install.sh --build-only, which fetches it).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/../.."
N="${NANOAPPS:-$ROOT/.nanoapps}"
H="${HOSTLV:-$ROOT/build/hostlv}"
[ -d "$N/lvgl/src" ] || { echo "no $N/lvgl/src - set NANOAPPS to a NanoApps checkout that has built once" >&2; exit 1; }
mkdir -p "$H"
cat > "$H/Makefile" <<EOF
NANO := $N
SRCS := \$(shell find \$(NANO)/lvgl/src -name '*.c')
OBJS := \$(patsubst \$(NANO)/lvgl/src/%.c,obj/%.o,\$(SRCS))
CFLAGS := -O1 -w -DHB_LV_RELOC -DLV_CONF_INCLUDE_SIMPLE -I\$(NANO)/sdk -I\$(NANO)/lvgl -I\$(NANO)/lvgl/src
all: liblvhost.a
obj/%.o: \$(NANO)/lvgl/src/%.c
	@mkdir -p \$(dir \$@)
	@cc -c \$(CFLAGS) \$< -o \$@
liblvhost.a: \$(OBJS)
	@ar rcs \$@ \$(OBJS)
EOF
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
make -C "$H" -j"$JOBS"
echo "built $H/liblvhost.a"
