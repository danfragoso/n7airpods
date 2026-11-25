#!/usr/bin/env bash
# run-harness.sh PATH/TO/name.bin — upload a fixed-VA harness to RAM and exec it.
#
# Sequence that matches the firmware's expectations:
#   1. eject the media over SCSI (START STOP UNIT 0x1b ... 02)  — the device stays attached
#   2. upload the .bin with C6/98 subcommand 01
#   3. exec at (link_va | 1) with C6/98 subcommand 03
#
# `diskutil unmountDisk` (what NanoApps' ./start uses) detaches the whole device on this
# dock, and uploading while the FAT volume is mounted gets SCSI status 0x04 and drops the
# device, so both are avoided here.
set -uo pipefail
BIN="${1:?usage: run-harness.sh <harness.bin>}"
SG="${IPOD_SG_RAW:-${NANOAPPS:-$HOME/NanoApps}/tools/macos/ipod_sg_raw}"
READELF="${READELF:-arm-none-eabi-readelf}"

ELF="${BIN%.bin}.elf"
if [ -n "${LINK_VA:-}" ]; then VA="$LINK_VA"
elif [ -f "$ELF" ]; then VA=$("$READELF" -S "$ELF" | awk '/\.text/ {print $5; exit}')
else echo "need LINK_VA (no $ELF)" >&2; exit 1; fi
VA_DEC=$((16#$VA))
SIZE=$(stat -f%z "$BIN")
addr() { local v=$1; printf '%02x %02x %02x %02x' $(( (v>>24)&255 )) $(( (v>>16)&255 )) $(( (v>>8)&255 )) $(( v&255 )); }

apple_present() { ioreg -p IOUSB -l -w0 2>/dev/null | grep -q '"idVendor" = 1452'; }
apple_present || { echo "no Apple USB device — replug the iPod" >&2; exit 1; }

echo "link_va=$VA (dec $VA_DEC)  size=$SIZE"
echo "==> eject media (device stays on the bus)"
"$SG" auto 1b 00 00 00 02 00 >/dev/null 2>&1 || true
sleep 1
echo "==> upload  c6 98 01 (512-byte chunks, as the firmware expects)"
TMP=$(mktemp); cp "$BIN" "$TMP"
PAD=$(( (SIZE + 511) / 512 * 512 ))
if [ "$PAD" -gt "$SIZE" ]; then dd if=/dev/zero bs=1 count=$((PAD-SIZE)) >>"$TMP" 2>/dev/null; fi
CHUNK=$(mktemp)
n=$(( PAD / 512 ))
for i in $(seq 0 $((n-1))); do
  dd if="$TMP" of="$CHUNK" bs=512 count=1 skip=$i 2>/dev/null
  cur=$(( VA_DEC + i*512 ))
  "$SG" -s 512 -i "$CHUNK" auto c6 98 01 $(addr "$cur") >/dev/null 2>&1 || { echo "chunk $i failed at $cur" >&2; rm -f "$TMP" "$CHUNK"; exit 1; }
done
rm -f "$TMP" "$CHUNK"
echo "    uploaded $PAD bytes in $n chunk(s)"
sleep 1
echo "==> exec    c6 98 03 at $VA_DEC|1"
"$SG" auto c6 98 03 $(addr $(( VA_DEC | 1 )))
rc=$?
echo "exec exit=$rc"
sleep 1
echo "==> reload media (leave the iPod usable)"
"$SG" auto 1b 00 00 00 03 00 >/dev/null 2>&1 || true
exit "$rc"
