#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BIN="$ROOT/build/test/placement_store_test"
if [[ ! -x "$BIN" ]]; then
  cmake --build build --target placement_store_test -j"$(nproc)"
fi
"$BIN"
echo "test_placement.sh PASS"
