#!/usr/bin/env bash
# 阶段 7：杀一个 Session，另一实例继续提供服务（委托 test_session_ha + 可选集群断言）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# 独立双 Session 演练（自起自停）
"$ROOT/scripts/test_session_ha.sh"

# 若本地集群在跑：再杀集群 session0，断言 session1 ready
RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/cluster}"
if [[ ! -f "$RUN_DIR/pids" ]]; then
  RUN_DIR="$ROOT/run/formal"
fi
HOST="${GAMEMESH_SMOKE_HOST:-127.0.0.1}"
S0="${GAMEMESH_SMOKE_SESSION0:-8093}"
S1="${GAMEMESH_SMOKE_SESSION1:-8096}"

if [[ -f "$RUN_DIR/pids" ]]; then
  mapfile -t PIDS <"$RUN_DIR/pids"
  # run_cluster_local 常见顺序：gw0 gw1 session0 session1 ... — 允许 SESSION0_PID 覆盖
  # start_formal [0]=session0；run_cluster 另追加 session2 到末尾 → S1 用 HTTP 8096
  S0_PID="${SESSION0_PID:-${PIDS[0]:-}}"
  if [[ -n "$S0_PID" ]] && kill -0 "$S0_PID" 2>/dev/null; then
    if curl -fsS -m 2 "http://${HOST}:${S1}/api/version" 2>/dev/null | grep -q session; then
      curl -fsS -m 3 "http://${HOST}:${S1}/health/ready" | grep -q '"ready":true' || true
      kill -9 "$S0_PID"
      sleep 1
      if curl -fsS -m 2 "http://${HOST}:${S0}/api/version" >/dev/null 2>&1; then
        echo "ERROR: killed session0 still answering" >&2
        exit 1
      fi
      curl -fsS -m 3 "http://${HOST}:${S1}/health/ready" | grep -q '"ready":true'
      echo "cluster session survivor ready"
    else
      echo "INFO: no session1 on ${S1}; HA covered by test_session_ha.sh only"
    fi
  fi
fi

echo "kill_session_drill.sh PASS"
