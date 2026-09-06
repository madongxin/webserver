#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
cmake --build build --target placement_store_test map_line_test map_dungeon_test map_p3_test -j"$(nproc)"
"$ROOT/build/test/placement_store_test"
"$ROOT/build/test/map_line_test"
"$ROOT/build/test/map_dungeon_test"
"$ROOT/build/test/map_p3_test"
echo "test_placement.sh PASS"
