#!/usr/bin/env bash
# 阶段 7：真实 SIGKILL Gateway-0，校验 Gateway-1 仍 ready（跨 GW 存活）
# 用法：先 ./scripts/run_cluster_local.sh（或 compose up），再执行本脚本
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/cluster}"
if [[ ! -f "$RUN_DIR/pids" ]]; then
  RUN_DIR="$ROOT/run/formal"
fi
HOST="${GAMEMESH_SMOKE_HOST:-127.0.0.1}"
GW0_HTTP="${GAMEMESH_SMOKE_GW0:-8080}"
GW1_HTTP="${GAMEMESH_SMOKE_GW1:-8082}"

if [[ ! -f "$RUN_DIR/pids" ]]; then
  echo "ERROR: cluster not running. Start: ./scripts/run_cluster_local.sh" >&2
  exit 1
fi

# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"
export GAMEMESH_RUN_DIR="$RUN_DIR"
GW0_PID="${GATEWAY0_PID:-}"
if [[ -z "$GW0_PID" ]]; then
  GW0_PID="$(e2e_pid_of gateway gw-0 2>/dev/null || true)"
fi
if [[ -z "$GW0_PID" ]]; then
  mapfile -t PIDS <"$RUN_DIR/pids"
  GW0_PID="${PIDS[6]:-}"
fi
GW1_HTTP_PORT="$(e2e_http_of gateway gw-1 2>/dev/null || echo "$GW1_HTTP")"
GW0_HTTP_PORT="$(e2e_http_of gateway gw-0 2>/dev/null || echo "$GW0_HTTP")"
if [[ -z "$GW0_PID" ]] || ! kill -0 "$GW0_PID" 2>/dev/null; then
  echo "ERROR: gateway0 pid invalid ($GW0_PID). Set GATEWAY0_PID= or check inventory" >&2
  exit 1
fi

curl -fsS -m 3 "http://${HOST}:${GW1_HTTP_PORT}/health/ready" | grep -q '"ready":true' || {
  echo "ERROR: gw1 not ready before kill" >&2
  exit 1
}

echo "drill: SIGKILL gateway0 pid=$GW0_PID; survivor http=${GW1_HTTP_PORT}"
kill -9 "$GW0_PID"
sleep 1
if kill -0 "$GW0_PID" 2>/dev/null; then
  echo "ERROR: gateway0 still alive after SIGKILL" >&2
  exit 1
fi
if curl -fsS -m 2 "http://${HOST}:${GW0_HTTP_PORT}/health/live" >/dev/null 2>&1; then
  echo "ERROR: killed gateway0 still answering" >&2
  exit 1
fi
curl -fsS -m 3 "http://${HOST}:${GW1_HTTP_PORT}/health/ready" | grep -q '"ready":true'
curl -fsS -m 3 "http://${HOST}:${GW1_HTTP_PORT}/api/version" | grep -q gateway
echo "kill_gateway_drill.sh PASS (gw0 dead, gw1 ready)"
