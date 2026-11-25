#!/bin/sh
# run_host_tests.sh - the unit tests that run on the desktop (no iPod needed).
#
#   tools/run_host_tests.sh                       uses ./.nanoapps (made by install.sh) for NanoApps' headers
#   NANOAPPS=/path/to/NanoApps tools/run_host_tests.sh
#
# The NanoApps checkout must have the AirPods patch applied (./install.sh --build-only does that).
# Exits non-zero if a test fails to build or fails.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
N="${NANOAPPS:-$ROOT/.nanoapps}"
[ -f "$N/system/silver_resident/compat_fixes.h" ] || { echo "set NANOAPPS to a patched NanoApps checkout (see install.sh --build-only)" >&2; exit 1; }
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT

echo "== AES-128 (FIPS-197 / SP 800-38A vectors)"
cc -O1 -Wall -Wextra -I integration -o "$T/aes" tools/test_aes128.c integration/aes128.c
"$T/aes"

echo "== BLE advertisement parser (captured AirPods Pro 2 advertisements + an encrypted synthetic one)"
cc -O1 -Wall -I tools/host_shim -I integration -I "$N/system/silver_resident" -I "$N/sdk" \
   -DREAL_ADVERTS_INC="\"$ROOT/tools/fixtures/real_adverts.inc\"" \
   -o "$T/le" tools/test_aap_le.c tools/host_shim/test_enc.c integration/aap_le.c integration/aes128.c
"$T/le"

echo "all host tests passed"
