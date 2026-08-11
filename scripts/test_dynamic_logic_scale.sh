#!/usr/bin/env bash
# 阶段二：真实启动 gl-2 + 进图 + DRAINING 后不再分配到 gl-2
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
SEED="${ROOT}/build/test/placement_seed_tool"
LOGIC_BIN="${ROOT}/build/test/gamelogic"
[[ -x "$CLIENT" && -x "$SEED" && -x "$LOGIC_BIN" ]] || {
  echo "ERROR: missing client/seed/gamelogic (build Debug)"; exit 1
}

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME}"
GW1="${E2E_GW1_GAME}"
HTTP_L2="${GAMEMESH_HTTP_L2:-19097}"
LOGIC2="${GAMEMESH_LOGIC2:-19203}"
L0="${GAMEMESH_LOGIC0:-19201}"
L1="${GAMEMESH_LOGIC1:-19202}"
GL2_LOG="$GAMEMESH_RUN_DIR/logs/logic2.log"
PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"
REDIS_PASS=""
[[ -f "$ROOT/config/redis.cnf" ]] && REDIS_PASS="$(awk -F= '/^password=/{print $2; exit}' "$ROOT/config/redis.cnf")"
RCLI=(redis-cli)
[[ -n "$REDIS_PASS" ]] && RCLI=(redis-cli -a "$REDIS_PASS" --no-auth-warning)

GL2_PID=""
cleanup_gl2() {
  if [[ -n "${GL2_PID:-}" ]] && kill -0 "$GL2_PID" 2>/dev/null; then
    kill -9 "$GL2_PID" 2>/dev/null || true
  fi
  "${RCLI[@]}" DEL "${PREFIX}svc:gamelogic:gl-2" >/dev/null 2>&1 || true
  "${RCLI[@]}" SREM "${PREFIX}svcidx:gamelogic" gl-2 >/dev/null 2>&1 || true
}
trap cleanup_gl2 EXIT

echo "== start gl-2 http=$HTTP_L2 rpc=$LOGIC2 =="
GAMEMESH_INSTANCE_ID=gl-2 nohup "$LOGIC_BIN" "$HTTP_L2" "$LOGIC2" >"$GL2_LOG" 2>&1 &
GL2_PID=$!
echo "$GL2_PID" >>"$GAMEMESH_RUN_DIR/pids"
e2e_inv_append gamelogic gl-2 "$GL2_PID" "127.0.0.1:${LOGIC2}" "$HTTP_L2" -

ready=0
for _ in $(seq 1 40); do
  if grep -qE 'GameLogicBrpcServer listening|role=gamelogic' "$GL2_LOG" 2>/dev/null && \
     curl -fsS -m 2 "http://${HOST}:${HTTP_L2}/health/live" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.5
done
[[ "$ready" -eq 1 ]] || { echo "ERROR: gl-2 not ready"; tail -40 "$GL2_LOG" || true; exit 1; }

echo "== wait redis register + gateway/session discover gl-2 =="
seen=0
for _ in $(seq 1 30); do
  st="$("${RCLI[@]}" HGET "${PREFIX}svc:gamelogic:gl-2" status 2>/dev/null || true)"
  addr="$("${RCLI[@]}" HGET "${PREFIX}svc:gamelogic:gl-2" address 2>/dev/null || true)"
  if [[ "$st" == "UP" && "$addr" == *":${LOGIC2}"* ]]; then
    seen=1
    break
  fi
  sleep 1
done
[[ "$seen" -eq 1 ]] || {
  echo "ERROR: gl-2 not in redis registry (status/address)"; exit 1
}
# Session/Gateway poll ≈5s；再留一轮给 ApplySnapshot
sleep 6

MAP_TPL="$((950000 + RANDOM % 1000))"
echo "== seed map preferred gl-2 tpl=$MAP_TPL =="
seed_out="$("$SEED" "$MAP_TPL" "gl-2" 1)"
echo "$seed_out"
MAP_ID="$(echo "$seed_out" | sed -n 's/^map_instance_id=//p' | head -1)"
[[ -n "$MAP_ID" ]] || { echo "ERROR: seed gl-2 failed"; exit 1; }
owner="$("${RCLI[@]}" HGET "${PREFIX}map:inst:${MAP_ID}" ownerLogicServerId || true)"
[[ "$owner" == "gl-2" ]] || { echo "ERROR: owner=$owner want gl-2"; exit 1; }

echo "== enter_map on gl-2 owned instance =="
out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$MAP_TPL" "$MAP_ID")"
echo "$out"
echo "$out" | grep -q 'enter_map_ok=1' || { echo "ERROR: enter_map on gl-2 failed"; exit 1; }
echo "$out" | grep -q 'dual_gw_ok=1' || { echo "ERROR: dual_gw after gl-2 enter failed"; exit 1; }

echo "== mark gl-2 DRAINING; Discover 不得返回 UP =="
"${RCLI[@]}" HSET "${PREFIX}svc:gamelogic:gl-2" status DRAINING >/dev/null
st="$("${RCLI[@]}" HGET "${PREFIX}svc:gamelogic:gl-2" status)"
[[ "$st" == "DRAINING" ]] || { echo "ERROR: DRAINING not set"; exit 1; }
sleep 6  # Session 刷新 owners（Discover 仅 UP）

MAP_TPL2="$((951000 + RANDOM % 1000))"
echo "== seed with preferred=gl-2 but SEED_OWNERS without gl-2 =="
seed2="$(SEED_OWNERS=gl-0,gl-1 "$SEED" "$MAP_TPL2" "gl-2" 1)"
echo "$seed2"
own2="$(echo "$seed2" | sed -n 's/^owner=//p' | head -1)"
[[ "$own2" != "gl-2" ]] || { echo "ERROR: DRAINING gl-2 still got new map"; exit 1; }
[[ "$own2" == "gl-0" || "$own2" == "gl-1" ]] || {
  echo "ERROR: unexpected owner=$own2"; exit 1
}

echo "== stop gl-2 =="
kill -9 "$GL2_PID" 2>/dev/null || true
GL2_PID=""
e2e_inv_remove gamelogic gl-2 2>/dev/null || true
"${RCLI[@]}" DEL "${PREFIX}svc:gamelogic:gl-2" >/dev/null || true
"${RCLI[@]}" SREM "${PREFIX}svcidx:gamelogic" gl-2 >/dev/null || true
trap - EXIT

echo "test_dynamic_logic_scale.sh PASS (L0=$L0 L1=$L1 L2=$LOGIC2)"
