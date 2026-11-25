#!/usr/bin/env bash
# Import the decrypted osos into a Ghidra project at its real load address.
#
#   tools/ghidra-import.sh build/osos.bin
#   tools/ghidra-import.sh build/osos.bin --noanalysis   # fast smoke test
#
# Then open the project in the GUI:
#   JAVA_HOME=/opt/homebrew/opt/openjdk@25 tooling/ghidra_12.2_PUBLIC/ghidraRun
#
# osos is a raw image (no ELF header) at 0x08000000, ARMv7 / Thumb-2.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:?usage: tools/ghidra-import.sh build/osos.bin [--noanalysis]}"
shift || true
# The decrypted file carries a 0x400-byte IMG1 payload header that is NOT loaded.
# Verified live over SCSI: RAM = 0x08000000 + (file_offset - 0x400).
#   file 0x0ceadc "BT L2CAP TXLoop"        -> RAM 0x080ce6dc  (peek: matches)
#   file 0x4e6d00 "L2CAP initialization…"  -> RAM 0x084e6900  (peek: matches)
#   file 0x2b2fd6 NanoBtFix original block -> RAM 0x082b2bd6  (NanoBtFix's published address)
# So map the file at 0x08000000 - 0x400 to make every address in Ghidra a real RAM address.
BASE=0x07ffffc00
PROJ="$HERE/build/ghidra"
mkdir -p "$PROJ"

exec "$HERE/research/tools/ghidra.sh" "$PROJ" nano-aap \
    -import "$BIN" \
    -processor "ARM:LE:32:v7" \
    -loader BinaryLoader \
    -loader-baseAddr "$BASE" \
    -analysisTimeoutPerFile 1800 \
    "$@"
