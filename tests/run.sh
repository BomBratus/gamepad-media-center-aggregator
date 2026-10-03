#!/usr/bin/env bash
# Standalone GMCA media, account and Stremio logic tests.
#
#   ./tests/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INC_APP="$ROOT/app/include"
INC_JSON="${GMCA_JSON_INCLUDE:-$ROOT/library/borealis/library/include/borealis/extern}"
CXX="${CXX:-c++}"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
rc=0

for src in "$ROOT"/tests/test_*.cpp; do
    name="$(basename "$src" .cpp)"
    bin="$OUT/$name"
    if [[ "$name" == test_imdb_index ]]; then
        cmake -S "$ROOT/tests/imdb" -B "$OUT/imdb" -DGMCA_JSON_INCLUDE="$INC_JSON" \
            ${GMCA_TEST_DEPENDENCY_ARGS:-} >/dev/null
        cmake --build "$OUT/imdb" -j2 >/dev/null
        "$OUT/imdb/test_imdb_index" || rc=1
        continue
    fi
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
