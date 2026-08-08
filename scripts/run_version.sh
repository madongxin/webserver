#!/usr/bin/env bash
# 打印各服务版本信息：Git SHA、构建类型、C++ 标准
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY="$(git status --porcelain 2>/dev/null | head -1 | wc -l | tr -d ' ')"
BUILD_DIR="${GAMEMESH_BUILD_DIR:-${MMO_BUILD_DIR:-$ROOT/build}}"
BUILD_TYPE="unknown"
if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  BUILD_TYPE="$(grep -E '^CMAKE_BUILD_TYPE:' "${BUILD_DIR}/CMakeCache.txt" | head -1 | cut -d= -f2 || true)"
  CXX_STD="$(grep -E '^CMAKE_CXX_STANDARD:' "${BUILD_DIR}/CMakeCache.txt" | head -1 | cut -d= -f2 || echo 17)"
else
  CXX_STD=17
fi

echo "GameMesh version"
echo "  git_sha     : ${GIT_SHA}${GIT_DIRTY:+ (dirty)}"
echo "  build_type  : ${BUILD_TYPE}"
echo "  cxx_standard: C++${CXX_STD}"
echo "  binaries:"
for b in gateway session gamelogic world gamedb server; do
  p="${ROOT}/build/test/${b}"
  if [[ -x "$p" ]]; then
    echo "    ${b}: $(ls -l "$p" | awk '{print $5,$6,$7,$8}')"
  else
    echo "    ${b}: (missing)"
  fi
done
