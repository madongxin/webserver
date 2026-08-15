#!/usr/bin/env bash
# S2：双 GW 重连世界快照、Push gap 恢复、非法坐标拒绝、GameLogic 重启后 last_safe。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: missing $CLIENT" >&2; exit 1; }

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/unity-e2e}"
export GAMEMESH_MAP_SHA256_FILE="${GAMEMESH_MAP_SHA256_FILE:-$ROOT/config/maps/map_1001.json.sha256}"
export GAMEMESH_MAP_DATA_VERSION="${GAMEMESH_MAP_DATA_VERSION:-1}"
export GAMEMESH_SAFE_POS_FLUSH_MS="${GAMEMESH_SAFE_POS_FLUSH_MS:-0}"

e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:?}"
GW1="${E2E_GW1_GAME:?}"

require() {
  local hay="$1" needle="$2"
  echo "$hay" | grep -q "$needle" || {
    echo "ERROR: missing $needle" >&2
    echo "$hay" >&2
    exit 1
  }
}

echo "== s2-world-recovery =="
set +e
out="$("$CLIENT" s2-world-recovery "$HOST" "$GW0" "$HOST" "$GW1" 1001)"
rc=$?
set -e
echo "$out"
[[ "$rc" -eq 0 ]] || { echo "ERROR: s2-world-recovery rc=$rc" >&2; exit "$rc"; }
require "$out" 's2_world_recovery_ok=1'
require "$out" 'reconnect_sees_peer=1'
require "$out" 'illegal_pos_rejected=1'
require "$out" 'gap_snapshot_ok=1'
require "$out" 'post_snap_move_ok=1'
require "$out" 'stale_seq_rejected=1'
require "$out" 'old_fence_rejected=1'

echo "== last-safe-save =="
set +e
save="$("$CLIENT" last-safe-save "$HOST" "$GW0")"
rc=$?
set -e
echo "$save"
[[ "$rc" -eq 0 ]] || { echo "ERROR: last-safe-save rc=$rc" >&2; exit "$rc"; }
player="$(echo "$save" | sed -n 's/^player_id=//p' | head -1)"
sid="$(echo "$save" | sed -n 's/^session_id=//p' | head -1)"
tok="$(echo "$save" | sed -n 's/^token=//p' | head -1)"
wantx="$(echo "$save" | sed -n 's/^saved_x=//p' | head -1)"
[[ -n "$player" && -n "$sid" && -n "$tok" && -n "$wantx" ]] || {
  echo "ERROR: last-safe-save missing fields" >&2
  exit 1
}

LOGIC_BIN=""
if [[ -x "$ROOT/build/test/gamelogic" ]]; then
  LOGIC_BIN="$ROOT/build/test/gamelogic"
elif [[ -x "$ROOT/build/test/server" ]]; then
  LOGIC_BIN="$ROOT/build/test/server"
fi
[[ -n "$LOGIC_BIN" ]] || { echo "ERROR: missing gamelogic binary" >&2; exit 1; }

PID_L0="$(e2e_pid_of gamelogic gl-0)" || { echo "ERROR: gl-0 missing" >&2; exit 1; }
HTTP_L0="$(e2e_http_of gamelogic gl-0)"
RPC_L0="$(e2e_rpc_of gamelogic gl-0)"
LOGIC_PORT="${RPC_L0##*:}"
echo "== restart gl-0 pid=$PID_L0 =="
kill -TERM "$PID_L0" 2>/dev/null || true
sleep 1
if kill -0 "$PID_L0" 2>/dev/null; then
  kill -9 "$PID_L0" 2>/dev/null || true
  sleep 1
fi
mkdir -p "$GAMEMESH_RUN_DIR/logs"
if [[ "$(basename "$LOGIC_BIN")" == "server" ]]; then
  GAMEMESH_FORMAL=1 GAMEMESH_INSTANCE_ID=gl-0 nohup "$LOGIC_BIN" gamelogic "$HTTP_L0" "$LOGIC_PORT" \
    >>"$GAMEMESH_RUN_DIR/logs/logic0.log" 2>&1 &
else
  GAMEMESH_FORMAL=1 GAMEMESH_INSTANCE_ID=gl-0 nohup "$LOGIC_BIN" "$HTTP_L0" "$LOGIC_PORT" \
    >>"$GAMEMESH_RUN_DIR/logs/logic0.log" 2>&1 &
fi
newpid=$!
echo "$newpid" >>"$GAMEMESH_RUN_DIR/pids"
e2e_inv_replace gamelogic gl-0 "$newpid" "$RPC_L0" "$HTTP_L0" -
ready=0
for _ in $(seq 1 50); do
  if kill -0 "$newpid" 2>/dev/null && (echo >/dev/tcp/127.0.0.1/"$LOGIC_PORT") >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.2
done
[[ "$ready" -eq 1 ]] || { echo "ERROR: gl-0 did not come back" >&2; exit 1; }
sleep 2
e2e_wait_login "$CLIENT" "$HOST" "$GW0"

echo "== last-safe-verify =="
set +e
ver="$("$CLIENT" last-safe-verify "$HOST" "$GW0" "$player" "$sid" "$tok" "$wantx")"
rc=$?
set -e
echo "$ver"
[[ "$rc" -eq 0 ]] || { echo "ERROR: last-safe-verify rc=$rc" >&2; exit "$rc"; }
require "$ver" 'last_safe_restored=1'

echo "test_world_snapshot.sh PASS"
