#!/usr/bin/env bash
# peek.sh ADDR [BYTES] — read live DRAM over SCSI (needs NanoApps' ipod_sg_raw compiled).
#   tools/peek.sh 0x084e9ab0 64
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
SG="${IPOD_SG_RAW:-${NANOAPPS:-$HOME/NanoApps}/tools/macos/ipod_sg_raw}"
ADDR=$(printf '%d' "${1:?addr}")
N=$(printf '%d' "${2:-64}")
OUT=$(mktemp)
for off in $(seq 0 $(( (N+511)/512*512 )) 512); do :; done
: > "$OUT"
a=$ADDR
rem=$N
while [ "$rem" -gt 0 ]; do
  b0=$(( (a>>24)&0xff )); b1=$(( (a>>16)&0xff )); b2=$(( (a>>8)&0xff )); b3=$(( a&0xff ))
  for try in 1 2 3 4 5 6 7 8; do
    "$SG" -o /tmp/.peek512 -r 512 auto c6 98 02 $b0 $b1 $b2 $b3 >/dev/null 2>&1 || true
    if [ -s /tmp/.peek512 ] && od -An -tx1 /tmp/.peek512 | tr -d ' \n' | grep -qv '^0*$'; then break; fi
    sleep 0.3
  done
  take=$(( rem < 512 ? rem : 512 ))
  dd if=/tmp/.peek512 bs=1 count=$take 2>/dev/null >> "$OUT"
  a=$(( a + take )); rem=$(( rem - take ))
done
xxd -l "$N" "$OUT"
rm -f "$OUT"
