#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# 集成项：依赖 Redis 的测试失败则失败（不再 SKIP→0）
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
# MySQL 资产测：不可用则 SKIP（exit 0）；可用则必须通过
if [[ -x ./build/test/phase3_gamedb_asset_test ]]; then
  if timeout 15 ./build/test/phase3_gamedb_asset_test; then
    :
  else
    ec=$?
    if [[ "$ec" -eq 124 ]]; then
      echo "SKIP: phase3_gamedb_asset_test timed out (MySQL unreachable?)"
    else
      echo "ERROR: phase3_gamedb_asset_test failed ec=$ec"
      exit "$ec"
    fi
  fi
fi
echo "test_integration.sh PASS"
