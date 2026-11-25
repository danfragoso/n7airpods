#!/usr/bin/env bash
# decrypt-resume.sh — wear down a flaky USB link instead of fighting it.
#
# wInd3x's `decrypt -r <file>` appends plaintext to a recovery buffer as it goes and
# resumes from that length on the next run. So a dock that drops the device mid-transfer
# is not fatal: each attempt keeps the progress of the last one. This script loops
# attempt -> resume until the image is complete, waiting for DFU to come back each time.
#
#   tools/decrypt-resume.sh [max-attempts]
set -uo pipefail

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
WIND3X="$HERE/tooling/bin/wInd3x"
IN="$HERE/build/mse/osos"
OUT="$HERE/build/osos.bin"
REC="$HERE/build/decrypt.recovery"
MAX="${1:-40}"
TOTAL=$(stat -f%z "$IN" 2>/dev/null || echo 0)

[ -f "$IN" ] || { echo "missing $IN — run tools/decrypt.sh once first" >&2; exit 1; }
mkdir -p "$HERE/build"

dfu_present() { ioreg -p IOUSB -l -w0 2>/dev/null | grep -q '"idProduct" = 4660'; }
have() { [ -f "$REC" ] && stat -f%z "$REC" || echo 0; }

pct() { awk -v a="$(have)" -v b="$TOTAL" 'BEGIN{ if (b>0) printf "%.1f", a*100/b; else print "0" }'; }

for attempt in $(seq 1 "$MAX"); do
    # wait up to 3 minutes for the device to be there
    for _ in $(seq 1 90); do
        dfu_present && break
        printf "\r[attempt %d] waiting for DFU (re-enter DFU if the nano rebooted)   " "$attempt"
        sleep 2
    done
    if ! dfu_present; then echo; echo "no DFU device — stop, re-enter DFU, re-run"; exit 1; fi

    echo "[attempt $attempt] resuming at $(have) / $TOTAL bytes ($(pct)%)"
    "$WIND3X" decrypt "$IN" "$OUT" -r "$REC" > "$HERE/build/decrypt-attempt.log" 2>&1
    rc=$?
    echo "[attempt $attempt] exit=$rc at $(have) / $TOTAL bytes ($(pct)%)"

    if [ "$rc" -eq 0 ] && [ -f "$OUT" ]; then
        echo "DONE: $OUT"
        ls -la "$OUT"
        shasum -a 256 "$OUT" | tee "$OUT.sha256"
        exit 0
    fi
    # a clean "no device" drop is expected; anything else, still retry — progress is kept
    sleep 3
done

echo "gave up after $MAX attempts at $(pct)% — rerun to keep going (progress is preserved)"
exit 2
