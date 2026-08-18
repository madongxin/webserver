#!/usr/bin/env bash
# 双 TCP 客户端：同图 AOI Enter/Move、断线重连恢复、Logout Leave。
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
GW1="${E2E_GW1_GAME:?}"
e2e_wait_login "$CLIENT" "$HOST" "$GW0"

set +e
out="$("$CLIENT" two-player-aoi "$HOST" "$GW0" "$HOST" "$GW1" 1001)"
rc=$?
set -e
echo "$out"
[[ "$rc" -eq 0 ]] || { echo "ERROR: two-player-aoi rc=$rc" >&2; exit "$rc"; }

require() {
  echo "$out" | grep -q "$1" || {
    echo "ERROR: missing $1" >&2
    exit 1
  }
}

require 'a_player_id='
require 'b_player_id='
require 'login_ok=1'
require 'profile_complete=1'
require 'same_instance=1'
require 'same_owner=1'
require 'a_aoi_enter_b=1'
require 'b_aoi_enter_a=1'
require 'a_enter_b_player_id='
require 'b_enter_a_player_id='
require 'a_enter_b_entity_id='
require 'b_enter_a_entity_id='
require 'a_enter_b_name='
require 'b_enter_a_name='
require 'a_enter_b_state_seq='
require 'b_enter_a_state_seq='
require 'a_enter_b_map_instance_id='
require 'b_enter_a_map_instance_id='
require 'aoi_move_ok=1'
require 'a_move_from_x='
require 'a_move_to_x='
require 'b_move_to_x='
require 'a_aoi_move_state_seq='
require 'b_aoi_move_state_seq='
require 'mail_e2e_ok=1'
require 'reconnect_ok=1'
require 'reconnect_aoi_restored=1'
require 'duplicate_entity=0'
require 'logout_ok=1'
require 'logout_idempotent=1'
require 'logout_stale_move_rejected=1'
require 'b_aoi_move_after_a_logout=0'
require 'b_aoi_leave_on_logout=1'
require 'session_released=1'
require 'b_logout_ok=1'
require 'two_player_aoi_ok=1'

echo "test_two_player_aoi.sh PASS"
