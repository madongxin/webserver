#!/usr/bin/env bash
# 阶段 4 门禁：真实 SIGKILL Owner GameLogic，再 MarkRecovering+Migrate 校验更高 epoch
# 用法：
#   ./scripts/run_cluster_local.sh   # 先有 gl-0 READY 地图，或设置 MAP_ID
#   ./scripts/kill_logic_drill.sh
#   MAP_ID=123 NEW_OWNER=gl-1 ./scripts/kill_logic_drill.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/cluster}"
if [[ ! -f "$RUN_DIR/pids" ]]; then
  RUN_DIR="$ROOT/run/formal"
fi
PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"
NEW_OWNER="${NEW_OWNER:-gl-1}"
LEASE_WAIT="${LEASE_WAIT:-1}"
DRILL_BIN="$ROOT/build/test/map_lease_drill"

if [[ ! -f "$RUN_DIR/pids" ]]; then
  echo "ERROR: cluster not running. Start: ./scripts/run_cluster_local.sh" >&2
  exit 1
fi
if [[ ! -x "$DRILL_BIN" ]]; then
  echo "ERROR: missing $DRILL_BIN (./scripts/build.sh Debug)" >&2
  exit 1
fi
if ! command -v redis-cli >/dev/null 2>&1; then
  echo "ERROR: redis-cli required" >&2
  exit 1
fi

mapfile -t PIDS <"$RUN_DIR/pids"
if (( ${#PIDS[@]} < 6 )); then
  echo "ERROR: unexpected pid count=${#PIDS[@]}" >&2
  exit 1
fi
LOGIC0_PID="${PIDS[4]}"
if ! kill -0 "$LOGIC0_PID" 2>/dev/null; then
  echo "ERROR: logic0 pid=$LOGIC0_PID not alive" >&2
  exit 1
fi

MAP_ID="${MAP_ID:-}"
if [[ -z "$MAP_ID" ]]; then
  MAP_ID="$(redis-cli --scan --pattern "${PREFIX}map:inst:*" | while read -r k; do
    owner="$(redis-cli HGET "$k" ownerLogicServerId)"
    state="$(redis-cli HGET "$k" state)"
    if [[ "$owner" == "gl-0" && "$state" == "READY" ]]; then
      echo "${k##*:}"
      break
    fi
  done || true)"
fi
if [[ -z "$MAP_ID" ]]; then
  echo "ERROR: no READY map owned by gl-0; EnterMap first or set MAP_ID=" >&2
  exit 1
fi

KEY="${PREFIX}map:inst:${MAP_ID}"
OLD_EPOCH="$(redis-cli HGET "$KEY" ownerEpoch)"
echo "drill: map=$MAP_ID old_epoch=$OLD_EPOCH SIGKILL logic0=$LOGIC0_PID -> $NEW_OWNER"

kill -9 "$LOGIC0_PID"
sleep "$LEASE_WAIT"
if kill -0 "$LOGIC0_PID" 2>/dev/null; then
  echo "ERROR: process still alive after SIGKILL" >&2
  exit 1
fi

"$DRILL_BIN" "$MAP_ID" "$NEW_OWNER" "$OLD_EPOCH"

NEW_EPOCH="$(redis-cli HGET "$KEY" ownerEpoch)"
NEW_OWNER_GOT="$(redis-cli HGET "$KEY" ownerLogicServerId)"
echo "after: owner=$NEW_OWNER_GOT epoch=$NEW_EPOCH"
[[ "$NEW_OWNER_GOT" == "$NEW_OWNER" ]]
[[ "$NEW_EPOCH" -gt "$OLD_EPOCH" ]]
echo "kill_logic_drill.sh PASS (real SIGKILL + epoch bump; old heartbeat rejected)"
