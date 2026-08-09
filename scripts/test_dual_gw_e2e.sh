#!/usr/bin/env bash
# 双 Gateway TCP E2E：Login@gw0 → EnterMap → 断线 → Reconnect@gw1（可靠 Push 回放）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
SEED="${ROOT}/build/test/placement_seed_tool"
[[ -x "$CLIENT" ]] || { echo "ERROR: build game_tcp_e2e_client first"; exit 1; }

# 端口：优先 E2E 集群，其次 formal 默认
if [[ -f "${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}/E2E_PORTS.env" ]]; then
  # shellcheck disable=SC1090
  source "${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}/E2E_PORTS.env"
fi
GW0_HOST="${E2E_GW0_HOST:-127.0.0.1}"
GW1_HOST="${E2E_GW1_HOST:-127.0.0.1}"
GW0_GAME="${E2E_GW0_GAME:-${GAMEMESH_GAME_G0:-8081}}"
GW1_GAME="${E2E_GW1_GAME:-${GAMEMESH_GAME_G1:-8083}}"

# 默认不预置地图（由 EnterMap 新建）；TRANSFER=1 时预置 gl-1 测跨 Logic
MAP_TPL="${E2E_MAP_TPL:-$((910000 + RANDOM % 10000))}"
MAP_INST=0
if [[ "${E2E_TRANSFER:-0}" == "1" && -x "$SEED" ]]; then
  seed_out="$("$SEED" "$MAP_TPL" "gl-1" 1)" || seed_out=""
  if [[ -n "$seed_out" ]]; then
    MAP_INST="$(echo "$seed_out" | sed -n 's/^map_instance_id=//p' | head -1)"
    echo "seeded map_inst=$MAP_INST owner=gl-1 tpl=$MAP_TPL"
  fi
fi

echo "== dual-gw TCP: gw0=${GW0_HOST}:${GW0_GAME} gw1=${GW1_HOST}:${GW1_GAME} =="
out="$("$CLIENT" dual-gw "$GW0_HOST" "$GW0_GAME" "$GW1_HOST" "$GW1_GAME" "$MAP_TPL" "${MAP_INST:-0}")"
echo "$out"
echo "$out" | grep -q 'login_ok=1'
echo "$out" | grep -q 'enter_map_ok=1'
echo "$out" | grep -q 'reconnect_ok=1'
echo "$out" | grep -q 'dual_gw_ok=1'
# 回放或明确缺口标志
echo "$out" | grep -qE 'replay_n=[1-9]|need_full_snapshot=1|push_recv=1' || {
  echo "ERROR: expected push replay evidence" >&2
  exit 1
}
if [[ -n "${MAP_INST:-}" && "$MAP_INST" != "0" ]]; then
  echo "$out" | grep -q 'gamelogic_instance_id=gl-1' || {
    echo "ERROR: expected transfer to gl-1" >&2
    exit 1
  }
fi
echo "test_dual_gw_e2e.sh PASS"
