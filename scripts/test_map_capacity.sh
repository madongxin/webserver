#!/usr/bin/env bash
# 公共地图 50 人满员；第 51 人进入新实例；任何实例不超过 50。
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

set +e
out="$("$CLIENT" map-capacity-51 "$HOST" "$GW0" 1001 51)"
rc=$?
set -e
echo "$out"
[[ "$rc" -eq 0 ]] || { echo "ERROR: map-capacity-51 rc=$rc" >&2; exit "$rc"; }
echo "$out" | grep -q 'capacity_players=51' || { echo "ERROR: expected 51 players" >&2; exit 1; }
echo "$out" | grep -q 'capacity_instances=' || { echo "ERROR: missing instance count" >&2; exit 1; }
ninst="$(echo "$out" | sed -n 's/^capacity_instances=//p' | head -1)"
[[ "${ninst:-0}" -ge 2 ]] || { echo "ERROR: expected >=2 instances got=$ninst" >&2; exit 1; }
maxn="$(echo "$out" | sed -n 's/^max_instance_n=//p' | head -1)"
[[ "${maxn:-99}" -le 50 ]] || { echo "ERROR: instance over capacity max=$maxn" >&2; exit 1; }
echo "$out" | grep -q 'map_capacity_ok=1' || { echo "ERROR: map_capacity_ok missing" >&2; exit 1; }

echo "test_map_capacity.sh PASS"
