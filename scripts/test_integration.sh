#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# 集成项：依赖 Redis/MySQL 的必需测试；Redis 不可用必须失败（禁止 SKIP→成功）
need_redis=0
REDIS_PASS=""
if [[ -f "$ROOT/config/redis.cnf" ]]; then
  REDIS_PASS="$(grep -E '^password=' "$ROOT/config/redis.cnf" | head -1 | cut -d= -f2- || true)"
fi
if command -v redis-cli >/dev/null 2>&1; then
  if [[ -n "$REDIS_PASS" ]]; then
    redis-cli -a "$REDIS_PASS" ping 2>/dev/null | grep -q PONG && need_redis=1
  else
    redis-cli ping 2>/dev/null | grep -q PONG && need_redis=1
  fi
fi
if [[ "$need_redis" -ne 1 ]]; then
  echo "ERROR: Redis required for integration tests (redis-cli PING)"
  exit 1
fi
./scripts/test_placement.sh
./build/test/session_store_test
./build/test/player_transfer_test
./build/test/placement_recovery_test
./build/test/phase3_discovery_test
./scripts/test_push_reconnect.sh
./build/test/push_ack_redis_test
./build/test/push_full_snapshot_test

# 阶段一必需 MySQL 门禁：不可用必须失败
if [[ ! -x ./build/test/gamedb_snapshot_idempotency_test ]]; then
  echo "ERROR: missing gamedb_snapshot_idempotency_test"
  exit 1
fi
./build/test/gamedb_snapshot_idempotency_test
./build/test/gamedb_unknown_result_test
if [[ -x ./build/test/phase3_gamedb_asset_test ]]; then
  ./build/test/phase3_gamedb_asset_test
fi
echo "test_integration.sh PASS"
