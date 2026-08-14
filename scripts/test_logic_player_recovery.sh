#!/usr/bin/env bash
# 阶段二：真实玩家进 gl-1 地图、GameDB 已确认资产、SIGKILL gl-1、gl-0 更高 epoch、资产恢复、旧 epoch 拒绝
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

SEED="${ROOT}/build/test/placement_seed_tool"
CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
DRILL="${ROOT}/build/test/map_lease_drill"
TOOL="${ROOT}/build/test/gamedb_rpc_tool"
[[ -x "$SEED" && -x "$CLIENT" && -x "$DRILL" && -x "$TOOL" ]] || {
  echo "ERROR: missing seed/client/drill/gamedb_rpc_tool"; exit 1
}

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME}"
GW1="${E2E_GW1_GAME}"
RPC_DB1="$(e2e_rpc_of gamedb gamedb-1 || true)"
[[ -n "$RPC_DB1" && "$RPC_DB1" != "-" ]] || RPC_DB1="127.0.0.1:${E2E_DB1_RPC:-${GAMEMESH_RPC_D1:-}}"
if [[ -z "${RPC_DB1##*:}" || "$RPC_DB1" == "127.0.0.1:" ]]; then
  RPC_DB1="$(e2e_rpc_of gamedb gamedb-0)"
fi
[[ -n "$RPC_DB1" && "$RPC_DB1" != "-" ]] || { echo "ERROR: gamedb rpc addr missing"; exit 1; }

PID_L1="$(e2e_pid_of gamelogic gl-1)" || { echo "ERROR: gl-1 missing"; exit 1; }
kill -0 "$PID_L1" 2>/dev/null || { echo "ERROR: gl-1 not alive"; exit 1; }

PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"
REDIS_PASS=""
[[ -f "$ROOT/config/redis.cnf" ]] && REDIS_PASS="$(awk -F= '/^password=/{print $2; exit}' "$ROOT/config/redis.cnf")"
RCLI=(redis-cli)
[[ -n "$REDIS_PASS" ]] && RCLI=(redis-cli -a "$REDIS_PASS" --no-auth-warning)

MAP_TPL="$((931000 + RANDOM % 1000))"
echo "== seed READY map on gl-1 tpl=$MAP_TPL =="
seed_out="$("$SEED" "$MAP_TPL" "gl-1" 1)"
echo "$seed_out"
MAP_ID="$(echo "$seed_out" | sed -n 's/^map_instance_id=//p' | head -1)"
OLD_EPOCH="$(echo "$seed_out" | sed -n 's/^owner_epoch=//p' | head -1)"
[[ -n "$MAP_ID" ]] || { echo "ERROR: seed failed"; exit 1; }
KEY="${PREFIX}map:inst:${MAP_ID}"
[[ "$OLD_EPOCH" =~ ^[0-9]+$ ]] || OLD_EPOCH="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"

echo "== login + enter_map on gl-1 =="
enter_out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$MAP_TPL" "$MAP_ID")"
echo "$enter_out"
echo "$enter_out" | grep -q 'enter_map_ok=1' || { echo "ERROR: enter_map failed"; exit 1; }
echo "$enter_out" | grep -q 'gamelogic_instance_id=gl-1' || {
  echo "ERROR: expected gl-1 owner"; exit 1
}
player="$(echo "$enter_out" | sed -n 's/^player_id=//p' | head -1)"
[[ -n "$player" ]] || { echo "ERROR: missing player_id"; exit 1; }

echo "== GameDB confirmed GRANT =="
ikey="logic-rec-grant-${player}-$$"
mut="$("$TOOL" mutate "$RPC_DB1" "$player" "$ikey" GRANT 42 3)"
echo "$mut"
echo "$mut" | grep -q 'ok=1' || { echo "ERROR: grant failed"; exit 1; }
inv_before="$("$TOOL" inventory "$RPC_DB1" "$player" 42)"
echo "$inv_before"
echo "$inv_before" | grep -q 'ok=1' || { echo "ERROR: inventory before kill"; exit 1; }
count_before="$(echo "$inv_before" | sed -n 's/.*bag_count=\([0-9]*\).*/\1/p' | head -1)"
[[ "${count_before:-0}" -ge 3 ]] || { echo "ERROR: bag_count=$count_before want>=3"; exit 1; }

echo "== SIGKILL gl-1 pid=$PID_L1 =="
kill -9 "$PID_L1"
sleep 1
if kill -0 "$PID_L1" 2>/dev/null; then
  echo "ERROR: gl-1 still alive"; exit 1
fi
"${RCLI[@]}" DEL "${PREFIX}svc:gamelogic:gl-1" >/dev/null 2>&1 || true
"${RCLI[@]}" SREM "${PREFIX}svcidx:gamelogic" gl-1 >/dev/null 2>&1 || true
"${RCLI[@]}" HSET "$KEY" leaseUntil 1 >/dev/null || true

echo "== recover placement to gl-0 =="
"$DRILL" resolve-reclaim "$MAP_ID" "gl-0"
NEW_EPOCH="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"
NEW_OWNER="$("${RCLI[@]}" HGET "$KEY" ownerLogicServerId)"
[[ "$NEW_OWNER" == "gl-0" ]] || { echo "ERROR: owner=$NEW_OWNER want gl-0"; exit 1; }
[[ "$NEW_EPOCH" -gt "$OLD_EPOCH" ]] || {
  echo "ERROR: epoch not increased $OLD_EPOCH -> $NEW_EPOCH"; exit 1
}

echo "== post-kill re-enter on gl-0 =="
ok_post=0
out2=""
for attempt in $(seq 1 8); do
  set +e
  out2="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$MAP_TPL" "$MAP_ID" 2>&1)"
  rc=$?
  set -e
  echo "attempt=$attempt rc=$rc"
  echo "$out2"
  if [[ "$rc" -eq 0 ]] && echo "$out2" | grep -q 'enter_map_ok=1' && \
     echo "$out2" | grep -q 'gamelogic_instance_id=gl-0'; then
    ok_post=1
    break
  fi
  sleep 2
done
[[ "$ok_post" -eq 1 ]] || { echo "ERROR: post-recover enter on gl-0 failed"; exit 1; }

inv_after="$("$TOOL" inventory "$RPC_DB1" "$player" 42)"
echo "$inv_after"
echo "$inv_after" | grep -q 'ok=1' || { echo "ERROR: inventory after recover"; exit 1; }
count_after="$(echo "$inv_after" | sed -n 's/.*bag_count=\([0-9]*\).*/\1/p' | head -1)"
[[ "$count_after" -eq "$count_before" ]] || {
  echo "ERROR: asset rolled back or duplicated before=$count_before after=$count_after"
  exit 1
}

echo "== old epoch must not reclaim as owner =="
cur_ep="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"
[[ "$cur_ep" -gt "$OLD_EPOCH" ]] || { echo "ERROR: old epoch still current"; exit 1; }

echo "test_logic_player_recovery.sh PASS"
