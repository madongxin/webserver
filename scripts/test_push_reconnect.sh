#!/usr/bin/env bash
# 阶段 5：可靠 Push 回放（进程内 cache + Redis ReplayStore）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BIN="$ROOT/build/test"

run() {
  local name="$1"
  if [[ ! -x "$BIN/$name" ]]; then
    echo "ERROR missing $BIN/$name" >&2
    exit 1
  fi
  echo "== $name =="
  "$BIN/$name"
}

run push_replay_cache_test
run push_replay_store_test
echo "test_push_reconnect.sh PASS (cache + Redis ReplayStore; dual-GW TCP E2E still manual)"
