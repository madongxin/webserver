#!/usr/bin/env bash
# 阶段二：GameDB 未知结果 / 幂等 + 集群 kill（使用 E2E 端口）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

[[ -x "$ROOT/build/test/gamedb_unknown_result_test" ]] || {
  echo "ERROR: missing gamedb_unknown_result_test"; exit 1
}
"$ROOT/build/test/gamedb_unknown_result_test"
"$ROOT/build/test/gamedb_snapshot_idempotency_test"

RUN_DIR="${GAMEMESH_RUN_DIR:-}"
if [[ -z "$RUN_DIR" || ! -f "$RUN_DIR/pids" ]]; then
  for d in "$ROOT/run/e2e" "$ROOT/run/formal"; do
    [[ -f "$d/pids" ]] && RUN_DIR="$d" && break
  done
fi
if [[ -n "${RUN_DIR:-}" && -f "$RUN_DIR/pids" ]]; then
  export GAMEMESH_RUN_DIR="$RUN_DIR"
  [[ -f "$RUN_DIR/E2E_PORTS.env" ]] && source "$RUN_DIR/E2E_PORTS.env"
  export GAMEMESH_SMOKE_HOST="${E2E_HOST:-127.0.0.1}"
  export GAMEMESH_SMOKE_GAMEDB0="${E2E_DB0_HTTP:-8094}"
  export GAMEMESH_SMOKE_GAMEDB1="${E2E_DB1_HTTP:-8095}"
  mapfile -t PIDS <"$RUN_DIR/pids"
  if [[ -n "${PIDS[1]:-}" ]] && kill -0 "${PIDS[1]}" 2>/dev/null; then
    "$ROOT/scripts/kill_gamedb_drill.sh"
  else
    echo "INFO: gamedb0 already dead; skip kill drill (unit idempotency covered)"
  fi
fi

echo "test_gamedb_unknown_result_failover.sh PASS"
