#!/usr/bin/env bash
# Unity 协议契约：schema/地图 hash、完整 Profile、错误地图哈希拒绝、GameLogic 重启后资料不丢。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: missing $CLIENT (./scripts/build.sh Debug)" >&2; exit 1; }

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/unity-e2e}"
export GAMEMESH_MAP_SHA256_FILE="${GAMEMESH_MAP_SHA256_FILE:-$ROOT/config/maps/map_1001.json.sha256}"
export GAMEMESH_MAP_DATA_VERSION="${GAMEMESH_MAP_DATA_VERSION:-1}"

e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:?}"
e2e_wait_login "$CLIENT" "$HOST" "$GW0"

PROTO_OUT="${GAMEMESH_RUN_DIR}/protocol"
"$ROOT/scripts/export_unity_protocol.sh" "$PROTO_OUT"
[[ -s "$PROTO_OUT/game.proto.sha256" ]] || { echo "ERROR: missing proto sha256" >&2; exit 1; }
[[ -s "$PROTO_OUT/protocol_manifest.json" ]] || { echo "ERROR: missing protocol_manifest.json" >&2; exit 1; }
[[ -s "$GAMEMESH_MAP_SHA256_FILE" ]] || { echo "ERROR: missing map sha256" >&2; exit 1; }
echo "game_proto_sha256=$(tr -d ' \n' <"$PROTO_OUT/game.proto.sha256")"
echo "map_sha256=$(awk '{print $1; exit}' "$GAMEMESH_MAP_SHA256_FILE")"

require() {
  local hay="$1" needle="$2"
  echo "$hay" | grep -q "$needle" || {
    echo "ERROR: missing $needle" >&2
    echo "$hay" >&2
    exit 1
  }
}

DEVICE="unity-contract-$$"
set +e
out="$("$CLIENT" unity-contract-check "$HOST" "$GW0" "$DEVICE" e2epass1)"
rc=$?
set -e
echo "$out"
[[ "$rc" -eq 0 ]] || { echo "ERROR: unity-contract-check rc=$rc" >&2; exit "$rc"; }
require "$out" 'login_ok=1'
require "$out" 'profile_complete=1'
require "$out" 'map_hash_mismatch_rejected=1'
require "$out" 'enter_map_ok=1'
require "$out" 'unity_contract_ok=1'
echo "$out" | grep -q 'profile_name=player' && {
  echo "ERROR: profile overwritten by compiled default name" >&2
  exit 1
}

player="$(echo "$out" | sed -n 's/^player_id=//p' | head -1)"
name="$(echo "$out" | sed -n 's/^profile_name=//p' | head -1)"
ver="$(echo "$out" | sed -n 's/^profile_stats_version=//p' | head -1)"
[[ -n "$player" && -n "$name" && -n "$ver" ]] || { echo "ERROR: missing profile fields" >&2; exit 1; }

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
echo "== restart gl-0 pid=$PID_L0 http=$HTTP_L0 rpc=$RPC_L0 =="
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

reload=""
rc=1
for _ in $(seq 1 30); do
  set +e
  reload="$("$CLIENT" login-profile "$HOST" "$GW0" "$player" e2epass1 "$DEVICE-reload" 2>&1)"
  rc=$?
  set -e
  if [[ "$rc" -eq 0 ]]; then
    break
  fi
  sleep 0.5
done
echo "$reload"
[[ "$rc" -eq 0 ]] || { echo "ERROR: login-profile after gl-0 restart rc=$rc" >&2; exit "$rc"; }
require "$reload" 'login_ok=1'
require "$reload" "profile_name=${name}"
require "$reload" "profile_stats_version=${ver}"
echo "$reload" | grep -q 'profile_name=player' && {
  echo "ERROR: restart reloaded compiled defaults" >&2
  exit 1
}

echo "test_unity_contract.sh PASS"
