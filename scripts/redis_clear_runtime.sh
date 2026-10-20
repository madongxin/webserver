#!/usr/bin/env bash
# 清理 Formal/压测留下的 Redis 运行时态（Session ONLINE、分线占位、push/sessop）。
# 保留：friend 缓存、svc 发现、账号相关（非本前缀业务键）。
# 用法：
#   ./scripts/redis_clear_runtime.sh
#   GAMEMESH_REDIS_PREFIX=gamemesh:dev: ./scripts/redis_clear_runtime.sh
# 可选：CLEAR_GW_CLAIM=1 同时删 gw_claim（stop_formal 默认开；start 前也可开）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REDIS_CNF="${GAMEMESH_REDIS_CNF:-$ROOT/config/redis.cnf}"

if ! command -v redis-cli >/dev/null 2>&1; then
  echo "WARN: redis-cli missing, skip runtime clear"
  exit 0
fi
if [[ ! -f "$REDIS_CNF" ]]; then
  echo "WARN: no $REDIS_CNF, skip runtime clear"
  exit 0
fi

REDIS_PASS="$(grep -E '^password=' "$REDIS_CNF" | head -1 | cut -d= -f2- || true)"
REDIS_HOST="$(grep -E '^ip=' "$REDIS_CNF" | head -1 | cut -d= -f2- || true)"
REDIS_PORT="$(grep -E '^port=' "$REDIS_CNF" | head -1 | cut -d= -f2- || true)"
PFX="${GAMEMESH_REDIS_PREFIX:-}"
if [[ -z "$PFX" ]]; then
  PFX="$(grep -E '^key_prefix=' "$REDIS_CNF" | head -1 | cut -d= -f2- || true)"
fi
PFX="${PFX:-gamemesh:dev}"
[[ "$PFX" == *: ]] || PFX="${PFX}:"

CLI=(redis-cli -h "${REDIS_HOST:-127.0.0.1}" -p "${REDIS_PORT:-6379}")
if [[ -n "$REDIS_PASS" ]]; then
  CLI+=(-a "$REDIS_PASS" --no-auth-warning)
fi

patterns=(
  "${PFX}session:*"
  "${PFX}online:players"
  "${PFX}session:grace"
  "${PFX}transfer:deadlines"
  "${PFX}map:*"
  "${PFX}sessop:*"
  "${PFX}reconnect:pending:*"
  "${PFX}push:*"
)
if [[ "${CLEAR_GW_CLAIM:-0}" == "1" ]]; then
  patterns+=("${PFX}gw_claim:*")
fi

deleted=0
batch=()
flush_batch() {
  if ((${#batch[@]} == 0)); then
    return 0
  fi
  "${CLI[@]}" DEL "${batch[@]}" >/dev/null 2>&1 || true
  deleted=$((deleted + ${#batch[@]}))
  batch=()
}
for pat in "${patterns[@]}"; do
  while IFS= read -r key; do
    [[ -z "$key" ]] && continue
    batch+=("$key")
    if ((${#batch[@]} >= 200)); then
      flush_batch
    fi
  done < <("${CLI[@]}" --scan --pattern "$pat" 2>/dev/null || true)
  flush_batch
  if [[ "$pat" != *'*'* ]]; then
    "${CLI[@]}" DEL "$pat" >/dev/null 2>&1 || true
  fi
done

online="$("${CLI[@]}" SCARD "${PFX}online:players" 2>/dev/null || echo 0)"
echo "redis_clear_runtime prefix=${PFX} deleted≈${deleted} online_scard=${online}"
