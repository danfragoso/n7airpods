#!/usr/bin/env bash
# Survey the decrypted osos for the Bluetooth stack's L2CAP surface.
#
#   tools/survey.sh build/osos.bin
#
# Produces:
#   build/symbols.txt        every OI_* / BT::* / profile-layer string, deduped
#   build/l2cap-surface.txt  just the L2CAP / AVCTP / AVDTP / RFCOMM / SDP entries
#   stdout                   the G0 verdict inputs
set -euo pipefail

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OSOS="${1:?usage: tools/survey.sh build/osos.bin}"
[ -f "$OSOS" ] || { echo "no such file: $OSOS" >&2; exit 1; }

SYMS="$HERE/build/symbols.txt"
SURF="$HERE/build/l2cap-surface.txt"
mkdir -p "$HERE/build"

echo "==> image"; ls -la "$OSOS"

# Symbols are string-table style (blueth4w: "rich debug strings and C++ symbols"), not ELF symtab.
strings -a -n 6 "$OSOS" \
  | grep -E '^(OI_|BT::|A2DP|AVDTP|AVCTP|AVRCP|L2CAP|L2CA|SDP|RFCOMM|HCI|GAP|SDP_|BTA_|HFP|SCO)' \
  | sort -u > "$SYMS"

echo "==> $(wc -l < "$SYMS") stack-ish symbols"

grep -iE 'l2cap|l2ca|psm|avdtp|avctp|rfcomm|sdp' "$SYMS" > "$SURF" || true
echo "==> $(wc -l < "$SURF") L2CAP-adjacent entries"

echo
echo "=============== G0 CHECKLIST ==============="
echo
echo "-- any explicit L2CAP connect/open entry point?"
grep -iE 'l2cap.*(connect|open)|(connect|open).*l2cap|l2ca_(connect|open|datawrite|register)' "$SYMS" || echo "   (none by name)"
echo
echo "-- BLUEmagic OI_ L2CAP namespace:"
grep -E '^OI_L2CAP|L2CA_' "$SYMS" || echo "   (none — stack may be symbol-stripped at that layer)"
echo
echo "-- profile channel setup (what we will clone if there is no API):"
grep -iE 'avdtp|avctp|rfcomm' "$SYMS" | head -40 || true
echo
echo "-- evidence of PSM immediates to look for in Ghidra: 0x0001 SDP, 0x0003 RFCOMM, 0x0017 AVRCP, 0x0019 AVDTP"
echo
echo "wrote: $SYMS"
echo "wrote: $SURF"
