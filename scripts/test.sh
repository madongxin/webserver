#!/usr/bin/env bash
# 单元 / 集成测试入口（失败非零；禁止吞错变绿）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
MODE="${1:-unit}"

case "$MODE" in
  unit)
    # 统一走 fail-closed 单元套件（含 Reactor / PasswordHash / PushReplayCache）
    exec "$ROOT/scripts/test_unit.sh"
    ;;
  integration)
    exec "$ROOT/scripts/test_integration.sh"
    ;;
  lowlevel)
    # 不依赖 brpc 的低层 target（需先 ENABLE_BRPC=OFF 构建）
    BIN="${ROOT}/build/test"
    fail=0
    for name in reactor_unit_test password_hash_test push_replay_cache_test; do
      if [[ ! -x "$BIN/$name" ]]; then
        echo "FAIL missing binary: $BIN/$name" >&2
        fail=1
        continue
      fi
      echo "== $name =="
      "$BIN/$name" || fail=1
    done
    [[ "$fail" -eq 0 ]] || exit 1
    echo "test.sh lowlevel PASS"
    ;;
  *)
    echo "usage: $0 unit|integration|lowlevel" >&2
    exit 2
    ;;
esac
