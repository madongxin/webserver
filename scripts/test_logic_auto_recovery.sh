#!/usr/bin/env bash
# 阶段二：Logic Owner 故障 → PlacementRecoveryScheduler 自动接管
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export AUTO_RECOVER=1
export LEASE_WAIT="${LEASE_WAIT:-15}"
export NEW_OWNER="${NEW_OWNER:-gl-1}"

SEED="${ROOT}/build/test/placement_seed_tool"
CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$SEED" ]] || { echo "ERROR: missing placement_seed_tool"; exit 1; }
[[ -x "$CLIENT" ]] || { echo "ERROR: missing game_tcp_e2e_client"; exit 1; }

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
[[ -f "$RUN_DIR/pids" ]] || RUN_DIR="$ROOT/run/formal"
[[ -f "$RUN_DIR/pids" ]] || { echo "ERROR: start cluster first"; exit 1; }
export GAMEMESH_RUN_DIR="$RUN_DIR"
[[ -f "$RUN_DIR/E2E_PORTS.env" ]] && source "$RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:-8081}"
MAP_TPL="$((930000 + RANDOM % 1000))"

echo "== seed READY map on gl-0 tpl=$MAP_TPL =="
seed_out="$("$SEED" "$MAP_TPL" "gl-0" 1)"
echo "$seed_out"
MAP_ID="$(echo "$seed_out" | sed -n 's/^map_instance_id=//p' | head -1)"
[[ -n "$MAP_ID" ]] || { echo "ERROR: seed failed"; exit 1; }
export MAP_ID

# 业务进图一次，确保 lease 被 Owner 续上后再杀
out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "${E2E_GW1_GAME:-8083}" "$MAP_TPL" "$MAP_ID")" || true
echo "$out"
echo "$out" | grep -qE 'enter_map_ok=1|login_ok=1' || {
  echo "WARN: dual-gw enter soft-fail; continue with seeded map for auto recover"
}

"$ROOT/scripts/kill_logic_drill.sh"
echo "test_logic_auto_recovery.sh PASS"
