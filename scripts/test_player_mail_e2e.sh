#!/usr/bin/env bash
# 玩家邮件公网路径：A 发无附件邮件，B 通知或轮询后 MailGet 正文一致。
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
[[ "$rc" -eq 0 ]] || { echo "ERROR: player-mail two-player-aoi rc=$rc" >&2; exit "$rc"; }
echo "$out" | grep -q 'mail_send_ok=1' || { echo "ERROR: mail send failed" >&2; exit 1; }
echo "$out" | grep -q 'mail_get_ok=1' || { echo "ERROR: mail get failed" >&2; exit 1; }
echo "$out" | grep -q 'mail_get_body=hello-from-a' || { echo "ERROR: mail body mismatch" >&2; exit 1; }
echo "$out" | grep -q 'mail_e2e_ok=1' || { echo "ERROR: mail e2e missing" >&2; exit 1; }

echo "test_player_mail_e2e.sh PASS"
