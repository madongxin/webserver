#!/usr/bin/env bash
# SIGKILL gl-0；按 inventory 查 PID（兼容旧 pids[4]）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
export GAMEMESH_RUN_DIR="$RUN_DIR"
PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"
NEW_OWNER="${NEW_OWNER:-gl-1}"
LEASE_WAIT="${LEASE_WAIT:-1}"
DRILL_BIN="$ROOT/build/test/map_lease_drill"

REDIS_PASS=""
[[ -f "$ROOT/config/redis.cnf" ]] && REDIS_PASS="$(awk -F= '/^password=/{print $2; exit}' "$ROOT/config/redis.cnf")"
RCLI=(redis-cli)
[[ -n "$REDIS_PASS" ]] && RCLI=(redis-cli -a "$REDIS_PASS" --no-auth-warning)

[[ -f "$RUN_DIR/pids" ]] || { echo "ERROR: cluster not running"; exit 1; }
[[ -x "$DRILL_BIN" ]] || { echo "ERROR: missing map_lease_drill"; exit 1; }

LOGIC0_PID=""
if LOGIC0_PID="$(e2e_pid_of gamelogic gl-0 2>/dev/null)"; then
  :
else
  mapfile -t PIDS <"$RUN_DIR/pids"
  LOGIC0_PID="${PIDS[4]:-}"
fi
[[ -n "$LOGIC0_PID" ]] || { echo "ERROR: gl-0 pid missing"; exit 1; }
kill -0 "$LOGIC0_PID" 2>/dev/null || { echo "ERROR: gl-0 not alive"; exit 1; }

MAP_ID="${MAP_ID:-}"
if [[ -z "$MAP_ID" ]]; then
  MAP_ID="$("${RCLI[@]}" --scan --pattern "${PREFIX}map:inst:*" | while read -r k; do
    owner="$("${RCLI[@]}" HGET "$k" ownerLogicServerId)"
    state="$("${RCLI[@]}" HGET "$k" state)"
    if [[ "$owner" == "gl-0" && "$state" == "READY" ]]; then
      echo "${k##*:}"
      break
    fi
  done || true)"
fi
[[ -n "$MAP_ID" ]] || { echo "ERROR: no READY map on gl-0"; exit 1; }

KEY="${PREFIX}map:inst:${MAP_ID}"
OLD_EPOCH="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"
[[ "$OLD_EPOCH" =~ ^[0-9]+$ ]] || { echo "ERROR: bad epoch"; exit 1; }
echo "drill: map=$MAP_ID old_epoch=$OLD_EPOCH SIGKILL gl-0=$LOGIC0_PID -> $NEW_OWNER"

kill -9 "$LOGIC0_PID"
sleep "$LEASE_WAIT"
if kill -0 "$LOGIC0_PID" 2>/dev/null; then
  echo "ERROR: still alive"; exit 1
fi

# 立刻摘除死实例发现，避免 Session 继续把新登录 sticky 到 gl-0
"${RCLI[@]}" DEL "${PREFIX}svc:gamelogic:gl-0" >/dev/null 2>&1 || true
"${RCLI[@]}" SREM "${PREFIX}svcidx:gamelogic" gl-0 >/dev/null 2>&1 || true

# 加速本 map 的 lease 过期
"${RCLI[@]}" HSET "$KEY" leaseUntil 1 >/dev/null || true

if [[ "${RESOLVE_RECLAIM:-0}" == "1" ]]; then
  # 稳定路径：不 MarkRecovering / 不启用实验调度器；靠 ResolveOrCreate 硬 reclaim
  "$DRILL_BIN" resolve-reclaim "$MAP_ID" "$NEW_OWNER"
elif [[ "${AUTO_RECOVER:-0}" == "1" ]]; then
  ok=0
  # 默认扫描积压时可能多轮 Tick；最多约 2 分钟
  for _ in $(seq 1 "${RECOVER_POLL_SEC:-120}"); do
    st="$("${RCLI[@]}" HGET "$KEY" state || true)"
    own="$("${RCLI[@]}" HGET "$KEY" ownerLogicServerId || true)"
    ep="$("${RCLI[@]}" HGET "$KEY" ownerEpoch || true)"
    if [[ "$st" == "READY" && "$own" == "$NEW_OWNER" && "$ep" =~ ^[0-9]+$ && "$ep" -gt "$OLD_EPOCH" ]]; then
      ok=1
      break
    fi
    sleep 1
  done
  [[ "$ok" -eq 1 ]] || {
    echo "ERROR: auto recover timeout state=$(${RCLI[@]} HGET "$KEY" state) owner=$(${RCLI[@]} HGET "$KEY" ownerLogicServerId) epoch=$(${RCLI[@]} HGET "$KEY" ownerEpoch)"
    exit 1
  }
else
  "$DRILL_BIN" "$MAP_ID" "$NEW_OWNER" "$OLD_EPOCH"
fi

NEW_EPOCH="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"
NEW_OWNER_GOT="$("${RCLI[@]}" HGET "$KEY" ownerLogicServerId)"
[[ "$NEW_OWNER_GOT" == "$NEW_OWNER" ]]
[[ "$NEW_EPOCH" -gt "$OLD_EPOCH" ]]
echo "kill_logic_drill.sh PASS auto=${AUTO_RECOVER:-0} resolve_reclaim=${RESOLVE_RECLAIM:-0}"
