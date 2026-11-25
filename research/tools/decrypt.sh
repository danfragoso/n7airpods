#!/usr/bin/env bash
# Decrypt the nano 7G's osos partition using the device's own AES engine (via wInd3x/S5Late).
# RAM-only on the device: nothing is written to the iPod. The nano must be in DFU mode.
#
#   tools/decrypt.sh ~/Downloads/iPod_1.1.2_39A10023.ipsw
#
# Prereqs:
#   - tooling/bin/wInd3x (built: `cd tooling && go build -o bin/wInd3x ...` or vendored)
#   - nano in DFU: hold Home+Sleep ~10s until the screen stays black; `lsusb`/system_profiler
#     shows 05ac:1234
#   - USB-A to Lightning cable (USB-C to Lightning does not reach DFU reliably — blueth4w)
#   - Apple's device daemons SIGSTOPped first, or they steal the DFU device within seconds:
#       sudo pkill -STOP -f MobileDeviceUpdater
#       sudo pkill -STOP -f AMPDevicesAgent
#       sudo pkill -STOP -f AMPDeviceDiscoveryAgent
#     (resume with -CONT afterwards)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
WIND3X="$HERE/tooling/bin/wInd3x"
IPSW="${1:?usage: tools/decrypt.sh <firmware.ipsw>}"
WORK="$HERE/build"
mkdir -p "$WORK/ipsw" "$WORK/mse"

[ -x "$WIND3X" ] || { echo "missing $WIND3X — build wInd3x first" >&2; exit 1; }

echo "==> extracting IPSW"
unzip -o -q "$IPSW" -d "$WORK/ipsw"
MSE="$(find "$WORK/ipsw" -name '*.MSE' -o -name '*.mse' | head -1)"
[ -n "$MSE" ] || { echo "no .MSE inside $IPSW" >&2; exit 1; }
echo "    $MSE"

echo "==> extracting MSE"
"$WIND3X" mse extract "$MSE" -o "$WORK/mse"

echo "==> MSE contents:"
ls -la "$WORK/mse"

# The OS image is the IMG1 (format 3) payload. Prefer a name match, else let the user pick.
# The IMG1 wrapper is `osos`; `osos.body` is the raw payload inside it. Take the wrapper.
IMG="$WORK/mse/osos"
[ -f "$IMG" ] || IMG="$(find "$WORK/mse" -iname 'osos' | head -1)"
if [ -z "$IMG" ]; then
  echo
  echo "Could not identify the osos image by name. Candidates:" >&2
  ls -la "$WORK/mse" >&2
  echo "Re-run: $WIND3X decrypt <candidate> $WORK/osos.bin" >&2
  exit 2
fi

echo "==> decrypting $IMG  (device must be in DFU)"
"$WIND3X" decrypt "$IMG" "$WORK/osos.bin"

echo "==> done"
ls -la "$WORK/osos.bin"
shasum -a 256 "$WORK/osos.bin" | tee "$WORK/osos.bin.sha256"
echo
echo "next: tools/survey.sh $WORK/osos.bin"
