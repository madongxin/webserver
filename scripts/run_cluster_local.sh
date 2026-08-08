#!/usr/bin/env bash
# 本地集群：2×gateway + 2×session + 2×gamelogic + 1×world + 2×gamedb
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/cluster}"
export GAMEMESH_ADVERTISE_HOST="${GAMEMESH_ADVERTISE_HOST:-127.0.0.1}"

# 复用 formal 端口布局，第二 session 用 8402
export GAMEMESH_SESSION="${GAMEMESH_SESSION:-8401}"
SESSION2="${GAMEMESH_SESSION2:-8402}"
HTTP_S2="${GAMEMESH_HTTP_S2:-8096}"

if [[ -f "$GAMEMESH_RUN_DIR/pids" ]]; then
  echo "cluster already running; ./scripts/stop_formal.sh with GAMEMESH_RUN_DIR=$GAMEMESH_RUN_DIR"
  exit 1
fi

# 先走 formal 启动（1 session），再追加 session2
./scripts/start_formal.sh

SESSION_BIN=""
if [[ -x "$ROOT/build/test/session" ]]; then
  SESSION_BIN="$ROOT/build/test/session"
elif [[ -x "$ROOT/build/test/server" ]]; then
  SESSION_BIN="$ROOT/build/test/server"
fi
if [[ -z "$SESSION_BIN" ]]; then
  echo "ERROR: session binary missing"
  exit 1
fi

mkdir -p "$GAMEMESH_RUN_DIR/logs"
if [[ "$(basename "$SESSION_BIN")" == "server" ]]; then
  nohup "$SESSION_BIN" session "$HTTP_S2" "$SESSION2" >"$GAMEMESH_RUN_DIR/logs/session2.log" 2>&1 &
else
  nohup "$SESSION_BIN" "$HTTP_S2" "$SESSION2" >"$GAMEMESH_RUN_DIR/logs/session2.log" 2>&1 &
fi
echo $! >>"$GAMEMESH_RUN_DIR/pids"
echo "started session2 pid=$! port=$SESSION2"
echo "run_cluster_local.sh PASS (2nd session on $SESSION2; Gateways still point to primary session_addrs)"
echo "NOTE: dual-session client LB for session_addrs 可在 gateway.cnf 配逗号列表（当前 Init 取首地址）"
