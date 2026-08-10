#!/usr/bin/env bash
# 阶段二：Session failover 业务连续性（非仅 HTTP）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: build game_tcp_e2e_client first"; exit 1; }

RUN_DIR="${GAMEMESH_RUN_DIR:-}"
if [[ -z "$RUN_DIR" || ! -f "$RUN_DIR/pids" ]]; then
  for d in "$ROOT/run/e2e" "$ROOT/run/formal" "$ROOT/run/cluster"; do
    [[ -f "$d/pids" ]] && RUN_DIR="$d" && break
  done
fi
[[ -f "$RUN_DIR/pids" ]] || { echo "ERROR: no cluster; run ./scripts/run_e2e_cluster.sh"; exit 1; }
export GAMEMESH_RUN_DIR="$RUN_DIR"
[[ -f "$RUN_DIR/E2E_PORTS.env" ]] && source "$RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:-${GAMEMESH_GAME_G0:-8081}}"
GW1="${E2E_GW1_GAME:-${GAMEMESH_GAME_G1:-8083}}"
S0_HTTP="${E2E_S0_HTTP:-8093}"
S1_HTTP="${E2E_S1_HTTP:-8096}"

mapfile -t PIDS <"$RUN_DIR/pids"
PID_S0="${PIDS[0]:-}"

echo "== pre: login on gw0 =="
dev="sessfail_$$"
out1="$("$CLIENT" register-login "$HOST" "$GW0" "$dev" "pw_sess_fail1")"
echo "$out1"
echo "$out1" | grep -q 'login_ok=1' || { echo "ERROR: pre login failed"; exit 1; }

echo "== kill session-0 =="
if [[ -z "$PID_S0" ]] || ! kill -0 "$PID_S0" 2>/dev/null; then
  echo "ERROR: session-0 pid missing/dead"; exit 1
fi
kill -9 "$PID_S0"
sleep 1
if curl -fsS -m 2 "http://${HOST}:${S0_HTTP}/api/version" >/dev/null 2>&1; then
  echo "ERROR: session-0 still answering" >&2
  exit 1
fi
curl -fsS -m 3 "http://${HOST}:${S1_HTTP}/api/version" | grep -q session

echo "== post-kill: new login via gw1 =="
out2="$("$CLIENT" register-login "$HOST" "$GW1" "sessfail_new_$$" "pw_new_ok")"
echo "$out2"
echo "$out2" | grep -q 'login_ok=1' || { echo "ERROR: post-kill login failed"; exit 1; }

echo "== post-kill: dual-gw reconnect business =="
out3="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$((920000 + RANDOM % 1000))" 0)"
echo "$out3"
echo "$out3" | grep -q 'reconnect_ok=1' || { echo "ERROR: reconnect_ok missing"; exit 1; }
echo "$out3" | grep -q 'dual_gw_ok=1' || { echo "ERROR: dual_gw_ok missing"; exit 1; }

echo "test_session_failover.sh PASS"
