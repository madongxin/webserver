#!/usr/bin/env bash
# 停止正式一键启动的进程，并尽量恢复 config 备份
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/formal}"

if [[ ! -f "$RUN_DIR/pids" ]]; then
  echo "没有运行中的正式实例（缺少 $RUN_DIR/pids）"
  exit 0
fi

echo "== GameMesh formal stop =="
while read -r pid; do
  [[ -z "$pid" ]] && continue
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    echo "  killed pid=$pid"
  fi
done <"$RUN_DIR/pids"

sleep 1
while read -r pid; do
  [[ -z "$pid" ]] && continue
  if kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null || true
    echo "  force killed pid=$pid"
  fi
done <"$RUN_DIR/pids"

rm -f "$RUN_DIR/pids"

# 释放 GatewayIdentity 短租约，避免重启同 ID 时 duplicate claim（TTL=120s）
if command -v redis-cli >/dev/null 2>&1 && [[ -f "$ROOT/config/redis.cnf" ]]; then
  REDIS_PASS="$(grep -E '^password=' "$ROOT/config/redis.cnf" | head -1 | cut -d= -f2- || true)"
  if [[ -n "$REDIS_PASS" ]]; then
    redis-cli -a "$REDIS_PASS" DEL 'gamemesh:dev:gw_claim:gw-0' 'gamemesh:dev:gw_claim:gw-1' >/dev/null 2>&1 || true
  else
    redis-cli DEL 'gamemesh:dev:gw_claim:gw-0' 'gamemesh:dev:gw_claim:gw-1' >/dev/null 2>&1 || true
  fi
fi

restore() {
  local name="$1"
  local dst="$ROOT/config/$name"
  local bak="$RUN_DIR/${name}.bak"
  if [[ -f "$bak" ]]; then
    mv -f "$bak" "$dst"
    echo "  restored config/$name"
  fi
}

restore gateway.cnf
restore world.cnf
restore session.cnf
restore gamedb.cnf
restore gamelogic.cnf

echo "已停止。客户端连接信息文件仍保留在: $RUN_DIR/CLIENT.txt"
