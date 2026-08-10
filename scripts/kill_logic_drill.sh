#!/usr/bin/env bash
# 阶段 4 门禁：真实 SIGKILL Owner GameLogic，再 MarkRecovering+Migrate 校验更高 epoch
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/cluster}"
if [[ ! -f "$RUN_DIR/pids" ]]; then
  RUN_DIR="$ROOT/run/formal"
fi
if [[ ! -f "$RUN_DIR/pids" ]]; then
  RUN_DIR="$ROOT/run/e2e"
fi
PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"
NEW_OWNER="${NEW_OWNER:-gl-1}"
LEASE_WAIT="${LEASE_WAIT:-1}"
DRILL_BIN="$ROOT/build/test/map_lease_drill"

REDIS_PASS=""
if [[ -f "$ROOT/config/redis.cnf" ]]; then
  REDIS_PASS="$(awk -F= '/^password=/{print $2; exit}' "$ROOT/config/redis.cnf")"
fi
RCLI=(redis-cli)
[[ -n "$REDIS_PASS" ]] && RCLI=(redis-cli -a "$REDIS_PASS" --no-auth-warning)

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
  MAP_ID="$("${RCLI[@]}" --scan --pattern "${PREFIX}map:inst:*" | while read -r k; do
    owner="$("${RCLI[@]}" HGET "$k" ownerLogicServerId)"
    state="$("${RCLI[@]}" HGET "$k" state)"
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
OLD_EPOCH="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"
[[ "$OLD_EPOCH" =~ ^[0-9]+$ ]] || { echo "ERROR: bad ownerEpoch='$OLD_EPOCH'"; exit 1; }
echo "drill: map=$MAP_ID old_epoch=$OLD_EPOCH SIGKILL logic0=$LOGIC0_PID -> $NEW_OWNER"

kill -9 "$LOGIC0_PID"
sleep "$LEASE_WAIT"
if kill -0 "$LOGIC0_PID" 2>/dev/null; then
  echo "ERROR: process still alive after SIGKILL" >&2
  exit 1
fi

if [[ "${AUTO_RECOVER:-0}" == "1" ]]; then
  echo "waiting auto PlacementRecoveryScheduler..."
  ok=0
  for _ in $(seq 1 40); do
    st="$("${RCLI[@]}" HGET "$KEY" state || true)"
    own="$("${RCLI[@]}" HGET "$KEY" ownerLogicServerId || true)"
    ep="$("${RCLI[@]}" HGET "$KEY" ownerEpoch || true)"
    if [[ "$st" == "READY" && "$own" == "$NEW_OWNER" && "$ep" =~ ^[0-9]+$ && "$ep" -gt "$OLD_EPOCH" ]]; then
      ok=1
      break
    fi
    sleep 1
  done
  if [[ "$ok" -ne 1 ]]; then
    echo "ERROR: auto recover timeout state=$("${RCLI[@]}" HGET "$KEY" state) owner=$("${RCLI[@]}" HGET "$KEY" ownerLogicServerId)" >&2
    exit 1
  fi
else
  "$DRILL_BIN" "$MAP_ID" "$NEW_OWNER" "$OLD_EPOCH"
fi

NEW_EPOCH="$("${RCLI[@]}" HGET "$KEY" ownerEpoch)"
NEW_OWNER_GOT="$("${RCLI[@]}" HGET "$KEY" ownerLogicServerId)"
echo "after: owner=$NEW_OWNER_GOT epoch=$NEW_EPOCH"
[[ "$NEW_OWNER_GOT" == "$NEW_OWNER" ]]
[[ "$NEW_EPOCH" -gt "$OLD_EPOCH" ]]
echo "kill_logic_drill.sh PASS (real SIGKILL + epoch bump; auto=${AUTO_RECOVER:-0})"
