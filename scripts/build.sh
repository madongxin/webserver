#!/usr/bin/env bash
# 配置并编译（默认 Debug；可传 Release）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_TYPE="${1:-${GAMEMESH_BUILD_TYPE:-${MMO_BUILD_TYPE:-Debug}}}"
BUILD_DIR="${GAMEMESH_BUILD_DIR:-${MMO_BUILD_DIR:-$ROOT/build}}"
JOBS="${GAMEMESH_JOBS:-${MMO_JOBS:-$(nproc 2>/dev/null || echo 4)}}"
TARGET="${2:-}"

# 基线默认打开常用能力；可用环境变量覆盖
: "${ENABLE_MYSQL:=ON}"
: "${ENABLE_GAME_PROTOBUF:=ON}"
: "${ENABLE_REDIS:=ON}"
: "${ENABLE_BRPC:=ON}"
: "${ENABLE_ROCKSDB:=OFF}"
: "${ENABLE_ASAN:=OFF}"

echo "== configure: type=$BUILD_TYPE dir=$BUILD_DIR =="
cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DENABLE_MYSQL="$ENABLE_MYSQL" \
  -DENABLE_GAME_PROTOBUF="$ENABLE_GAME_PROTOBUF" \
  -DENABLE_REDIS="$ENABLE_REDIS" \
  -DENABLE_BRPC="$ENABLE_BRPC" \
  -DENABLE_ROCKSDB="$ENABLE_ROCKSDB" \
  -DENABLE_ASAN="$ENABLE_ASAN"

echo "== build: jobs=$JOBS target=${TARGET:-all} =="
if [[ -n "$TARGET" ]]; then
  cmake --build "$BUILD_DIR" --target "$TARGET" -j"$JOBS"
else
  cmake --build "$BUILD_DIR" -j"$JOBS"
fi

echo "== done: binaries under $ROOT/build/test/ (EXECUTABLE_OUTPUT_PATH) =="
ls -la "$ROOT/build/test/" 2>/dev/null | head -20 || true
