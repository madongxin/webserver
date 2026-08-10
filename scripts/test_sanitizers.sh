#!/usr/bin/env bash
# Sanitizer 专项：ASan / UBSan / TSan（分目录构建，互斥）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
WHICH="${1:-all}"

run_asan() {
  echo "== ASan =="
  local b="$ROOT/build-asan"
  rm -rf "$b"
  ENABLE_ASAN=ON ENABLE_BRPC=OFF ENABLE_MYSQL=OFF ENABLE_REDIS=ON \
    GAMEMESH_BUILD_DIR="$b" ./scripts/build.sh Debug
  ASAN_OPTIONS=detect_leaks=0 "$b/test/reactor_unit_test"
  ASAN_OPTIONS=detect_leaks=0 "$b/test/password_hash_test"
  ASAN_OPTIONS=detect_leaks=0 "$b/test/player_serial_async_test"
  echo "ASan PASS"
}

run_ubsan() {
  echo "== UBSan =="
  local b="$ROOT/build-ubsan"
  rm -rf "$b"
  ENABLE_UBSAN=ON ENABLE_BRPC=OFF ENABLE_MYSQL=OFF ENABLE_REDIS=ON \
    GAMEMESH_BUILD_DIR="$b" ./scripts/build.sh Debug
  UBSAN_OPTIONS=print_stacktrace=1 "$b/test/reactor_unit_test"
  UBSAN_OPTIONS=print_stacktrace=1 "$b/test/password_hash_test"
  echo "UBSan PASS"
}

run_tsan() {
  echo "== TSan =="
  local b="$ROOT/build-tsan"
  rm -rf "$b"
  local tsan_opts="halt_on_error=1:suppressions=${ROOT}/tools/tsan_suppressions.txt"
  if [[ -f /usr/local/include/brpc/server.h || -f /usr/include/brpc/server.h ]]; then
    # ENABLE_BRPC 要求 MYSQL=ON（见 CMakeLists）；需系统 libtsan 运行时
    ENABLE_TSAN=ON ENABLE_BRPC=ON ENABLE_MYSQL=ON ENABLE_REDIS=ON \
      GAMEMESH_BUILD_DIR="$b" ./scripts/build.sh Debug
    TSAN_OPTIONS="$tsan_opts" "$b/test/channel_snapshot_race_test"
    TSAN_OPTIONS="$tsan_opts" "$b/test/player_serial_async_test"
  else
    ENABLE_TSAN=ON ENABLE_BRPC=OFF ENABLE_MYSQL=OFF ENABLE_REDIS=ON \
      GAMEMESH_BUILD_DIR="$b" ./scripts/build.sh Debug
    TSAN_OPTIONS="$tsan_opts" "$b/test/player_serial_async_test"
    echo "WARN: brpc missing — TSan channel_snapshot_race_test skipped"
  fi
  echo "TSan PASS"
}

case "$WHICH" in
  asan) run_asan ;;
  ubsan) run_ubsan ;;
  tsan) run_tsan ;;
  all) run_asan; run_ubsan; run_tsan ;;
  *) echo "usage: $0 [asan|ubsan|tsan|all]" >&2; exit 2 ;;
esac
echo "test_sanitizers.sh PASS ($WHICH)"
