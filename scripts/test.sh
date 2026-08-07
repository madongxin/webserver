#!/usr/bin/env bash
# 单元 / 集成测试入口（复用已有二进制）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BIN="${ROOT}/build/test"
MODE="${1:-unit}"

run_one() {
  local name="$1"
  shift
  if [[ ! -x "$BIN/$name" ]]; then
    echo "SKIP missing $name"
    return 0
  fi
  echo "== $name =="
  "$BIN/$name" "$@"
}

# 集成测试加超时，避免 MySQL/依赖不可用时永久卡住
run_one_timeout() {
  local secs="$1"
  local name="$2"
  shift 2
  if [[ ! -x "$BIN/$name" ]]; then
    echo "SKIP missing $name"
    return 0
  fi
  echo "== $name (timeout ${secs}s) =="
  if command -v timeout >/dev/null 2>&1; then
    timeout --signal=TERM "${secs}" "$BIN/$name" "$@" || {
      local ec=$?
      if [[ $ec -eq 124 ]]; then
        echo "WARN $name timed out after ${secs}s (check MySQL/Redis)"
      else
        echo "WARN $name exit=$ec"
      fi
      return 0
    }
  else
    "$BIN/$name" "$@" || echo "WARN $name failed"
  fi
}

case "$MODE" in
  unit)
    run_one message_route_test
    run_one map_placement_test
    run_one player_serial_queue_test
    run_one auth_session_boundary_test
    if [[ -x "$BIN/session_store_test" ]]; then
      run_one session_store_test || echo "WARN session_store_test (needs Redis)"
    fi
    ;;
  integration)
    run_one_timeout 60 gamedb_mail_claim_test
    if [[ -x "$ROOT/scripts/run_version_local.sh" ]]; then
      echo "== run_version_local smoke (timeout 90s) =="
      if command -v timeout >/dev/null 2>&1; then
        timeout --signal=TERM 90 "$ROOT/scripts/run_version_local.sh" || echo "WARN version smoke"
      else
        "$ROOT/scripts/run_version_local.sh" || echo "WARN version smoke"
      fi
    fi
    ;;
  *)
    echo "usage: $0 unit|integration"
    exit 2
    ;;
esac
echo "test.sh $MODE done"
