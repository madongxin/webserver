#!/usr/bin/env bash
# 配置并编译（默认 Debug；可传 Release / clean）
# 用法：
#   ./scripts/build.sh Debug
#   ./scripts/build.sh Release
#   ./scripts/build.sh clean
#   ENABLE_BRPC=OFF ./scripts/build.sh Debug   # 低层单测构建
#   ./scripts/build.sh Debug --lowlevel
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_TYPE="${1:-${GAMEMESH_BUILD_TYPE:-${MMO_BUILD_TYPE:-Debug}}}"
# Release 默认独立目录，避免覆盖 Debug 的 build/test 业务二进制
if [[ -n "${GAMEMESH_BUILD_DIR:-${MMO_BUILD_DIR:-}}" ]]; then
  BUILD_DIR="${GAMEMESH_BUILD_DIR:-${MMO_BUILD_DIR}}"
elif [[ "$BUILD_TYPE" == "Release" ]]; then
  BUILD_DIR="$ROOT/build-release"
else
  BUILD_DIR="$ROOT/build"
fi
JOBS="${GAMEMESH_JOBS:-${MMO_JOBS:-$(nproc 2>/dev/null || echo 4)}}"
TARGET=""
LOWLEVEL=0

shift_args=()
if [[ $# -ge 1 ]]; then
  shift || true
fi
while [[ $# -gt 0 ]]; do
  case "$1" in
    --lowlevel) LOWLEVEL=1 ;;
    *)
      if [[ -z "$TARGET" ]]; then
        TARGET="$1"
      else
        shift_args+=("$1")
      fi
      ;;
  esac
  shift || true
done

if [[ "${BUILD_TYPE}" == "clean" ]]; then
  echo "== clean: rm -rf ${BUILD_DIR} =="
  rm -rf "${BUILD_DIR}"
  exit 0
fi

if [[ "$LOWLEVEL" -eq 1 ]]; then
  : "${ENABLE_BRPC:=OFF}"
  : "${ENABLE_MYSQL:=ON}"
  : "${ENABLE_GAME_PROTOBUF:=ON}"
  : "${ENABLE_REDIS:=ON}"
else
  : "${ENABLE_MYSQL:=ON}"
  : "${ENABLE_GAME_PROTOBUF:=ON}"
  : "${ENABLE_REDIS:=ON}"
  : "${ENABLE_BRPC:=ON}"
fi
: "${ENABLE_ROCKSDB:=OFF}"
: "${ENABLE_ASAN:=OFF}"
: "${ENABLE_UBSAN:=OFF}"
: "${ENABLE_TSAN:=OFF}"

if [[ "${ENABLE_ASAN}" == "ON" && "${ENABLE_TSAN}" == "ON" ]]; then
  echo "ERROR: ENABLE_ASAN and ENABLE_TSAN are mutually exclusive" >&2
  exit 1
fi

if [[ "${ENABLE_BRPC}" == "ON" ]]; then
  DEPS_PREFIX="${GAMEMESH_DEPS_PREFIX:-$HOME/.local/gamemesh-deps}"
  if [[ ! -f /usr/local/include/brpc/server.h && ! -f /usr/include/brpc/server.h && \
        ! -f "$DEPS_PREFIX/include/brpc/server.h" ]]; then
    echo "ERROR: ENABLE_BRPC=ON but brpc headers not found." >&2
    echo "       Install via ./scripts/install_deps.sh --build-brpc, or: ./scripts/build.sh ${BUILD_TYPE} --lowlevel" >&2
    exit 1
  fi
  if [[ -f "$DEPS_PREFIX/include/brpc/server.h" ]]; then
    export CMAKE_PREFIX_PATH="${DEPS_PREFIX}${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    export CPATH="${DEPS_PREFIX}/include${CPATH:+:$CPATH}"
    export LIBRARY_PATH="${DEPS_PREFIX}/lib:${DEPS_PREFIX}/lib64${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export LD_LIBRARY_PATH="${DEPS_PREFIX}/lib:${DEPS_PREFIX}/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  fi
fi

CXX_BIN="${CXX:-g++}"
echo "== toolchain =="
echo "  compiler : ${CXX_BIN}"
${CXX_BIN} --version | head -1 || true
echo "  build    : ${BUILD_TYPE}"
echo "  cxx std  : C++17 (CMAKE_CXX_STANDARD=17)"
echo "  dir      : ${BUILD_DIR}"
echo "  brpc     : ${ENABLE_BRPC}"
echo "  asan     : ${ENABLE_ASAN}"
echo "  ubsan    : ${ENABLE_UBSAN}"
echo "  tsan     : ${ENABLE_TSAN}"

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
  -DENABLE_ASAN="$ENABLE_ASAN" \
  -DENABLE_UBSAN="$ENABLE_UBSAN" \
  -DENABLE_TSAN="$ENABLE_TSAN"

echo "== build: jobs=$JOBS target=${TARGET:-all} =="
if [[ -n "$TARGET" ]]; then
  cmake --build "$BUILD_DIR" --target "$TARGET" -j"$JOBS"
else
  cmake --build "$BUILD_DIR" -j"$JOBS"
fi

echo "== done: binaries under ${BUILD_DIR}/test/ =="
ls -la "${BUILD_DIR}/test/" 2>/dev/null | head -20 || true
