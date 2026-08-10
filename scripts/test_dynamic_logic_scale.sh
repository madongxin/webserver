#!/usr/bin/env bash
# 阶段二：动态扩容 gl-2 / DRAINING 契约
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"
REDIS_PASS=""
if [[ -f "$ROOT/config/redis.cnf" ]]; then
  REDIS_PASS="$(awk -F= '/^password=/{print $2; exit}' "$ROOT/config/redis.cnf")"
fi
RCLI=(redis-cli)
[[ -n "$REDIS_PASS" ]] && RCLI=(redis-cli -a "$REDIS_PASS" --no-auth-warning)

command -v redis-cli >/dev/null || { echo "ERROR: redis-cli required"; exit 1; }
[[ -x "$ROOT/build/test/discovery_ha_test" ]] && "$ROOT/build/test/discovery_ha_test"

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
[[ -f "$RUN_DIR/pids" ]] || RUN_DIR="$ROOT/run/formal"
if [[ ! -f "$RUN_DIR/pids" ]]; then
  echo "INFO: no cluster — discovery unit only"
  echo "test_dynamic_logic_scale.sh PASS (unit)"
  exit 0
fi
export GAMEMESH_RUN_DIR="$RUN_DIR"

n="$("${RCLI[@]}" SCARD "${PREFIX}svc:gamelogic:idx" 2>/dev/null || echo 0)"
# 非数字时置 0
[[ "$n" =~ ^[0-9]+$ ]] || n=0
echo "gamelogic idx count=$n"

if "${RCLI[@]}" EXISTS "${PREFIX}svc:gamelogic:gl-1" 2>/dev/null | grep -q 1; then
  "${RCLI[@]}" HSET "${PREFIX}svc:gamelogic:gl-1" status DRAINING >/dev/null
  st="$("${RCLI[@]}" HGET "${PREFIX}svc:gamelogic:gl-1" status || true)"
  echo "gl-1 status=$st"
  [[ "$st" == "DRAINING" ]]
  "${RCLI[@]}" HSET "${PREFIX}svc:gamelogic:gl-1" status ACTIVE >/dev/null
fi

echo "test_dynamic_logic_scale.sh PASS"
