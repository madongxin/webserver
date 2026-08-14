#!/usr/bin/env bash
# 阶段二：同一活动 Session 在 SIGKILL gw0 后经 gw1 Reconnect（禁止新账号 Login 代替）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: build game_tcp_e2e_client"; exit 1; }

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME}"
GW1="${E2E_GW1_GAME}"
PID_GW0="$(e2e_pid_of gateway gw-0)" || { echo "ERROR: gw-0 missing"; exit 1; }
kill -0 "$PID_GW0" 2>/dev/null || { echo "ERROR: gw-0 not alive"; exit 1; }

out="$(mktemp)"
cleanup() { rm -f "$out"; }
trap cleanup EXIT

echo "== hold-kill-reconnect gw0=$GW0 gw1=$GW1 pid_gw0=$PID_GW0 =="
"$CLIENT" hold-kill-reconnect "$HOST" "$GW0" "$HOST" "$GW1" >"$out" 2>&1 &
cpid=$!
ready=0
for _ in $(seq 1 120); do
  if grep -q 'READY_TO_KILL=1' "$out" 2>/dev/null; then
    ready=1
    break
  fi
  if ! kill -0 "$cpid" 2>/dev/null; then
    break
  fi
  sleep 0.25
done
if [[ "$ready" -ne 1 ]]; then
  echo "ERROR: client never reached READY_TO_KILL"
  cat "$out"
  wait "$cpid" || true
  exit 1
fi
echo "SIGKILL gw0 pid=$PID_GW0 (TCP still held by client)"
kill -9 "$PID_GW0"
set +e
wait "$cpid"
rc=$?
set -e
cat "$out"
[[ "$rc" -eq 0 ]] || { echo "ERROR: hold-kill-reconnect rc=$rc"; exit 1; }
grep -q 'hold_kill_reconnect_ok=1' "$out" || { echo "ERROR: missing hold_kill_reconnect_ok"; exit 1; }
grep -q 'reconnect_ok=1' "$out" || { echo "ERROR: missing reconnect_ok"; exit 1; }
grep -q 'old_fence_still_equal=0' "$out" || { echo "ERROR: fence did not rotate"; exit 1; }
echo "test_gateway_active_reconnect.sh PASS"
