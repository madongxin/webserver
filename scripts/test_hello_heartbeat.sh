#!/usr/bin/env bash
# S1：Hello 强制、错误 schema/版本、心跳 RTT、心跳洪泛、idle 后 DISCONNECTED 可重连。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: missing $CLIENT" >&2; exit 1; }

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/unity-e2e}"
export GAMEMESH_IDLE_TIMEOUT_MS="${GAMEMESH_IDLE_TIMEOUT_MS:-2500}"
export GAMEMESH_HEARTBEAT_INTERVAL_MS="${GAMEMESH_HEARTBEAT_INTERVAL_MS:-500}"
export GAMEMESH_HEARTBEAT_MIN_INTERVAL_MS="${GAMEMESH_HEARTBEAT_MIN_INTERVAL_MS:-250}"
export GAMEMESH_HELLO_DEADLINE_MS="${GAMEMESH_HELLO_DEADLINE_MS:-2500}"
export GAMEMESH_MAP_SHA256_FILE="${GAMEMESH_MAP_SHA256_FILE:-$ROOT/config/maps/map_1001.json.sha256}"
export GAMEMESH_MAP_DATA_VERSION="${GAMEMESH_MAP_DATA_VERSION:-1}"

if [[ "${GAMEMESH_SKIP_CLUSTER_STOP:-0}" != "1" ]]; then
  "$ROOT/scripts/stop_unity_integration_server.sh" 2>/dev/null || true
fi
e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:?}"

run_cmd() {
  local name="$1"
  shift
  echo "== $name =="
  local out rc
  set +e
  out="$("$CLIENT" "$@" 2>&1)"
  rc=$?
  set -e
  echo "$out"
  [[ "$rc" -eq 0 ]] || { echo "ERROR: $name rc=$rc" >&2; exit "$rc"; }
  LAST_OUT="$out"
}

run_cmd client-hello client-hello "$HOST" "$GW0"
echo "$LAST_OUT" | grep -q 'map_manifest_version=1' || {
  echo "ERROR: client-hello missing map_manifest_version=1" >&2
  exit 1
}
echo "$LAST_OUT" | grep -q 'hello_maps_n=1' || {
  echo "ERROR: client-hello missing hello_maps_n=1" >&2
  exit 1
}
run_cmd hello-then-register register-login "$HOST" "$GW0" "e2e-s1-reg-$$" e2epass1
run_cmd hello-bad-version hello-reject-login "$HOST" "$GW0" version
run_cmd hello-bad-schema hello-reject-login "$HOST" "$GW0" schema
run_cmd heartbeat heartbeat "$HOST" "$GW0"
run_cmd heartbeat-flood heartbeat-flood "$HOST" "$GW0"
run_cmd idle-reconnect idle-reconnect "$HOST" "$GW0"

echo "test_hello_heartbeat.sh PASS"
