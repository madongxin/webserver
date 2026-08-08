#!/usr/bin/env bash
# 杀死一个 GameLogic 后，对指定 map 调用 MarkRecovering + Migrate（需 Redis + 运行中 session）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/formal}"
MAP_ID="${1:-}"
NEW_OWNER="${2:-gl-1}"

if [[ -z "$MAP_ID" ]]; then
  echo "usage: $0 <map_instance_id> [new_owner_logic_id]"
  exit 2
fi

LOGIC0_PID=""
if [[ -f "$RUN_DIR/pids" ]]; then
  # 启发式：kill 第一个 gamelogic 日志对应进程较复杂；此处杀 logic0 日志里的 pid 行
  echo "INFO: stop gamelogic0 via stop then manual recover, or kill by pid from $RUN_DIR/pids"
fi

# 通过 redis-cli 直接置 RECOVERING（与 PlacementStore Lua 字段兼容）
PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"
KEY="${PREFIX}map:inst:${MAP_ID}"
if command -v redis-cli >/dev/null 2>&1; then
  redis-cli HSET "$KEY" state RECOVERING >/dev/null
  echo "ok: set $KEY state=RECOVERING"
  echo "next: call SessionService.MigrateMap map=$MAP_ID new_owner=$NEW_OWNER (or restart logic)"
else
  echo "redis-cli missing; mark recovering skipped"
fi
echo "kill_logic_and_recover.sh done (MVP helper; see PHASE2_STATUS.md limits)"
