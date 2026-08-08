#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BIN="$ROOT/build/test/push_replay_cache_test"
if [[ ! -x "$BIN" ]]; then
  cmake --build build --target push_replay_cache_test -j"$(nproc)"
fi
"$BIN"
echo "test_push_reconnect.sh PASS (replay cache unit; E2E cross-GW TBD)"
