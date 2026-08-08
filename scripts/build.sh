#!/usr/bin/env bash
# 配置并编译（默认 Debug；可传 Release）；支持 clean
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_TYPE="${1:-${GAMEMESH_BUILD_TYPE:-${MMO_BUILD_TYPE:-Debug}}}"
BUILD_DIR="${GAMEMESH_BUILD_DIR:-${MMO_BUILD_DIR:-$ROOT/build}}"
JOBS="${GAMEMESH_JOBS:-${MMO_JOBS:-$(nproc 2>/dev/null || echo 4)}}"
TARGET="${2:-}"

if [[ "${BUILD_TYPE}" == "clean" ]]; then
  echo "== clean: rm -rf ${BUILD_DIR} =="
  rm -rf "${BUILD_DIR}"
  exit 0
fi

# 基线默认打开常用能力；可用环境变量覆盖
: "${ENABLE_MYSQL:=ON}"
: "${ENABLE_GAME_PROTOBUF:=ON}"
: "${ENABLE_REDIS:=ON}"
: "${ENABLE_BRPC:=ON}"
: "${ENABLE_ROCKSDB:=OFF}"
: "${ENABLE_ASAN:=OFF}"

CXX_BIN="${CXX:-g++}"
echo "== toolchain =="
echo "  compiler : ${CXX_BIN}"
${CXX_BIN} --version | head -1 || true
echo "  build    : ${BUILD_TYPE}"
echo "  cxx std  : C++17 (CMAKE_CXX_STANDARD=17)"
echo "  dir      : ${BUILD_DIR}"
echo "  asan     : ${ENABLE_ASAN}"

echo "== configure: type=$BUILD_TYPE dir=$BUILD_DIR =="
cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DCMAKE_CXX_EXTENSIONS=OFF \
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

echo "== done: binaries under $ROOT/build/test/ =="
ls -la "$ROOT/build/test/" 2>/dev/null | head -20 || true
