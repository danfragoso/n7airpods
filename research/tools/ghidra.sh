#!/usr/bin/env bash
# Wrapper: run Ghidra headless with the keg-only JDK 21 that brew installed.
#   tools/ghidra.sh <project-dir> <name> -import build/osos.bin -postScript ...
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@25}"
export PATH="$JAVA_HOME/bin:$PATH"
exec "$HERE/tooling/ghidra_12.2_PUBLIC/support/analyzeHeadless" "$@"
