#!/usr/bin/env bash
# 跨 gw0/gw1 重复登录：旧连接收到 SessionReplacedNotify 并关闭，新连接有效，旧 fence 拒绝。
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
export GAMEMESH_KICK_NOTIFY_GRACE_MS="${GAMEMESH_KICK_NOTIFY_GRACE_MS:-120}"

e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:?}"
GW1="${E2E_GW1_GAME:?}"
e2e_wait_login "$CLIENT" "$HOST" "$GW0"

set +e
out="$("$CLIENT" duplicate-login "$HOST" "$GW0" "$HOST" "$GW1" e2epass1)"
rc=$?
set -e
echo "$out"
[[ "$rc" -eq 0 ]] || { echo "ERROR: duplicate-login rc=$rc" >&2; exit "$rc"; }

require() {
  echo "$out" | grep -q "$1" || {
    echo "ERROR: missing $1" >&2
    exit 1
  }
}

require 'login_ok=1'
require 'kicked_previous=1'
require 'old_got_session_replaced=1'
require 'replaced_reason=SESSION_REPLACED'
require 'old_conn_closed=1'
require 'new_profile_ok=1'
require 'old_fence_rejected=1'
require 'generation_increased=1'
require 'duplicate_login_ok=1'

echo "test_duplicate_login_e2e.sh PASS"
