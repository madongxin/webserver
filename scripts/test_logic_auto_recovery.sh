#!/usr/bin/env bash
# 阶段二：Logic Owner 真实故障 → 自动恢复 → 再进图/命令成功
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

export AUTO_RECOVER=1
export LEASE_WAIT="${LEASE_WAIT:-5}"
export NEW_OWNER="${NEW_OWNER:-gl-1}"
export RECOVER_POLL_SEC="${RECOVER_POLL_SEC:-120}"

SEED="${ROOT}/build/test/placement_seed_tool"
CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$SEED" && -x "$CLIENT" ]] || { echo "ERROR: missing seed/client"; exit 1; }

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME}"
GW1="${E2E_GW1_GAME}"
MAP_TPL="$((930000 + RANDOM % 1000))"
PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"
REDIS_PASS=""
[[ -f "$ROOT/config/redis.cnf" ]] && REDIS_PASS="$(awk -F= '/^password=/{print $2; exit}' "$ROOT/config/redis.cnf")"
RCLI=(redis-cli)
[[ -n "$REDIS_PASS" ]] && RCLI=(redis-cli -a "$REDIS_PASS" --no-auth-warning)

echo "== prune stale map:inst keys (SCAN backlog breaks auto-recover E2E) =="
# 仅清理 placement 实例键；不动 session/online
del_n=0
while read -r k; do
  [[ -z "$k" ]] && continue
  "${RCLI[@]}" DEL "$k" >/dev/null || true
  del_n=$((del_n + 1))
done < <("${RCLI[@]}" --scan --pattern "${PREFIX}map:inst:*")
echo "pruned_map_inst=$del_n"

echo "== seed READY map on gl-0 tpl=$MAP_TPL =="
seed_out="$("$SEED" "$MAP_TPL" "gl-0" 1)"
echo "$seed_out"
MAP_ID="$(echo "$seed_out" | sed -n 's/^map_instance_id=//p' | head -1)"
[[ -n "$MAP_ID" ]] || { echo "ERROR: seed failed"; exit 1; }
export MAP_ID
KEY="${PREFIX}map:inst:${MAP_ID}"
OLD_EPOCH="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"
OLD_ROUTE="$("${RCLI[@]}" HGET "$KEY" routeVersion || echo 0)"
[[ "$OLD_EPOCH" =~ ^[0-9]+$ ]] || { echo "ERROR: bad epoch"; exit 1; }

echo "== business enter_map before kill =="
out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$MAP_TPL" "$MAP_ID")"
echo "$out"
echo "$out" | grep -q 'enter_map_ok=1' || { echo "ERROR: enter_map must succeed before kill"; exit 1; }
echo "$out" | grep -q 'login_ok=1' || { echo "ERROR: login_ok missing"; exit 1; }

"$ROOT/scripts/kill_logic_drill.sh"

NEW_EPOCH="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"
NEW_OWNER_GOT="$("${RCLI[@]}" HGET "$KEY" ownerLogicServerId)"
NEW_ROUTE="$("${RCLI[@]}" HGET "$KEY" routeVersion || echo 0)"
[[ "$NEW_OWNER_GOT" == "$NEW_OWNER" ]] || {
  echo "ERROR: owner=$NEW_OWNER_GOT want $NEW_OWNER"; exit 1
}
[[ "$NEW_EPOCH" -gt "$OLD_EPOCH" ]] || {
  echo "ERROR: epoch not increased ($OLD_EPOCH -> $NEW_EPOCH)"; exit 1
}
if [[ "$NEW_ROUTE" =~ ^[0-9]+$ && "$OLD_ROUTE" =~ ^[0-9]+$ ]]; then
  [[ "$NEW_ROUTE" -ge "$OLD_ROUTE" ]] || {
    echo "ERROR: routeVersion decreased"; exit 1
  }
fi
echo "recovered owner=$NEW_OWNER_GOT epoch=$OLD_EPOCH->$NEW_EPOCH route=$OLD_ROUTE->$NEW_ROUTE"

# 等 Session Discover 刷新 owners（去掉死 gl-0）
sleep 6

echo "== post-recover: re-enter on new owner via gw1 =="
# gl-0 已死；低配机器上 dual-gw 可能瞬时失败，带重试
ok_post=0
out2=""
for attempt in $(seq 1 8); do
  set +e
  out2="$("$CLIENT" dual-gw "$HOST" "$GW1" "$HOST" "$GW1" "$MAP_TPL" "$MAP_ID" 2>&1)"
  rc=$?
  set -e
  echo "attempt=$attempt rc=$rc"
  echo "$out2"
  if [[ "$rc" -eq 0 ]] && echo "$out2" | grep -q 'enter_map_ok=1' && \
     echo "$out2" | grep -q "gamelogic_instance_id=${NEW_OWNER}" && \
     echo "$out2" | grep -q 'dual_gw_ok=1'; then
    ok_post=1
    break
  fi
  sleep 2
done
[[ "$ok_post" -eq 1 ]] || { echo "ERROR: post-recover enter on ${NEW_OWNER} failed"; exit 1; }

# 旧 epoch 不得仍为 owner 写入（owner 已迁走且 epoch 更大）
cur_ep="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"
[[ "$cur_ep" -gt "$OLD_EPOCH" ]] || { echo "ERROR: old epoch still current"; exit 1; }

echo "test_logic_auto_recovery.sh PASS"
