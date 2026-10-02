#!/usr/bin/env bash
# Standalone GMCA media, account and Stremio logic tests.
#
#   ./tests/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INC_APP="$ROOT/app/include"
INC_JSON="$ROOT/library/borealis/library/include/borealis/extern"
CXX="${CXX:-c++}"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
rc=0

for src in "$ROOT"/tests/test_*.cpp; do
    name="$(basename "$src" .cpp)"
    bin="$OUT/$name"
    if ! "$CXX" -std=gnu++17 -Wall -I"$ROOT/tests/support" -I"$INC_APP" -I"$INC_JSON" "$src" -o "$bin"; then
        echo "COMPILE FAIL: $name"
        rc=1
        continue
    fi
    if ! "$bin"; then
        rc=1
    fi
done

exit "$rc"
