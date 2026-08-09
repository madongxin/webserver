#!/usr/bin/env bash
# 阶段 7：SIGKILL GameDB-0，断言依赖方不能假绿（Logic/Gateway ready 可能仍绿，但 gamedb HTTP 死）
# 并校验第二 gamedb（若存在）或恢复后可再探活。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/cluster}"
if [[ ! -f "$RUN_DIR/pids" ]]; then
  RUN_DIR="$ROOT/run/formal"
fi
HOST="${GAMEMESH_SMOKE_HOST:-127.0.0.1}"
DB0="${GAMEMESH_SMOKE_GAMEDB0:-8094}"
DB1="${GAMEMESH_SMOKE_GAMEDB1:-8095}"

if [[ ! -f "$RUN_DIR/pids" ]]; then
  echo "ERROR: cluster not running" >&2
  exit 1
fi

mapfile -t PIDS <"$RUN_DIR/pids"
# start_formal: [1]=gamedb0
DB0_PID="${GAMEDB0_PID:-${PIDS[1]:-}}"
if [[ -z "$DB0_PID" ]] || ! kill -0 "$DB0_PID" 2>/dev/null; then
  if command -v ss >/dev/null 2>&1; then
    DB0_PID="$(ss -lptn "sport = :${DB0}" 2>/dev/null | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1 || true)"
  fi
fi
if [[ -z "$DB0_PID" ]] || ! kill -0 "$DB0_PID" 2>/dev/null; then
  echo "ERROR: gamedb0 pid unresolved; set GAMEDB0_PID=" >&2
  exit 1
fi

curl -fsS -m 3 "http://${HOST}:${DB0}/health/ready" | grep -q '"ready":true' || {
  echo "ERROR: gamedb0 not ready before kill" >&2
  exit 1
}

echo "drill: SIGKILL gamedb0 pid=$DB0_PID"
kill -9 "$DB0_PID"
sleep 1
if kill -0 "$DB0_PID" 2>/dev/null; then
  echo "ERROR: gamedb0 still alive" >&2
  exit 1
fi
if curl -fsS -m 2 "http://${HOST}:${DB0}/health/live" >/dev/null 2>&1; then
  echo "ERROR: killed gamedb0 still answering" >&2
  exit 1
fi

# 若有 gamedb1：必须仍 ready（多实例分担）
if curl -fsS -m 2 "http://${HOST}:${DB1}/api/version" 2>/dev/null | grep -q gamedb; then
  curl -fsS -m 3 "http://${HOST}:${DB1}/health/ready" | grep -q '"ready":true'
  echo "gamedb1 survivor ready"
else
  echo "WARN: no gamedb1 on ${DB1}; single-node GameDB SPOF confirmed for this topology"
fi

echo "kill_gamedb_drill.sh PASS"
