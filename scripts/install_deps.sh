#!/usr/bin/env bash
# 可重复依赖引导（不自动 sudo）。固定版本；可重复执行。
# 用法:
#   ./scripts/install_deps.sh                 # 仅检查并打印系统包安装命令
#   ./scripts/install_deps.sh --build-brpc    # 下载并编译 brpc 到 PREFIX
#   ./scripts/install_deps.sh --check-only
#
# 环境:
#   GAMEMESH_DEPS_PREFIX   默认 $HOME/.local/gamemesh-deps
#   GAMEMESH_BRPC_VERSION  默认 1.9.0
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PREFIX="${GAMEMESH_DEPS_PREFIX:-$HOME/.local/gamemesh-deps}"
BRPC_VER="${GAMEMESH_BRPC_VERSION:-1.9.0}"
# apache/brpc 1.9.0 tarball sha256 (github tags/1.9.0); bump with version
BRPC_SHA256="${GAMEMESH_BRPC_SHA256:-85856da0216773e1296834116f69f9e80007b7ff421db3be5c9d1890ecfaea74}"
SKIP_CHECKSUM=0
BUILD_BRPC=0
CHECK_ONLY=0
for a in "$@"; do
  case "$a" in
    --build-brpc) BUILD_BRPC=1 ;;
    --check-only) CHECK_ONLY=1 ;;
    --skip-checksum) SKIP_CHECKSUM=1 ;;
    -h|--help)
      echo "usage: $0 [--check-only|--build-brpc] [--skip-checksum]"
      exit 0
      ;;
  esac
done

log() { echo "== [install_deps] $* =="; }
warn() { echo "WARN: $*" >&2; }

print_system_packages() {
  cat <<'EOF'
系统包（需管理员手动安装，本脚本不执行 sudo）:

  Debian/Ubuntu:
    sudo apt-get update
    sudo apt-get install -y g++ cmake make pkg-config protobuf-compiler libprotobuf-dev \
      libssl-dev libjsoncpp-dev libhiredis-dev libmysqlclient-dev \
      libgflags-dev libleveldb-dev zlib1g-dev curl wget git \
      libasan6 libubsan1 libtsan0 shellcheck

  RHEL/Alibaba/CentOS:
    sudo yum install -y gcc-c++ cmake make pkgconfig openssl-devel jsoncpp-devel \
      protobuf-devel mysql-devel hiredis-devel gflags-devel leveldb-devel zlib-devel \
      curl wget git libasan libubsan libtsan ShellCheck

固定版本建议:
  - C++17: GCC >= 10 或 Clang >= 12
  - protobuf: 与仓库生成的 *.pb.cc 匹配（当前 3.x）
  - brpc: 1.9.0（可用 --build-brpc 安装到 PREFIX）
  - sanitizers: libasan / libubsan / libtsan 运行时（TSan 链接需要）
EOF
}

log "check existing toolchain"
./scripts/check_deps.sh --full || true
print_system_packages

if [[ "$CHECK_ONLY" -eq 1 ]]; then
  log "check-only done"
  exit 0
fi

if [[ "$BUILD_BRPC" -ne 1 ]]; then
  if [[ -f /usr/local/include/brpc/server.h || -f /usr/include/brpc/server.h ]]; then
    log "brpc already present system-wide; nothing to build"
    exit 0
  fi
  if [[ -f "$PREFIX/include/brpc/server.h" ]]; then
    log "brpc already in PREFIX=$PREFIX"
    echo "export PATH=\"$PREFIX/bin:\$PATH\""
    echo "export CMAKE_PREFIX_PATH=\"$PREFIX:\${CMAKE_PREFIX_PATH:-}\""
    echo "export CPATH=\"$PREFIX/include:\${CPATH:-}\""
    echo "export LIBRARY_PATH=\"$PREFIX/lib:$PREFIX/lib64:\${LIBRARY_PATH:-}\""
    echo "export LD_LIBRARY_PATH=\"$PREFIX/lib:$PREFIX/lib64:\${LD_LIBRARY_PATH:-}\""
    exit 0
  fi
  warn "brpc missing. Re-run with --build-brpc after installing system packages."
  exit 1
fi

mkdir -p "$PREFIX" "$ROOT/.deps/src"
SRC="$ROOT/.deps/src/brpc-${BRPC_VER}"
TGZ="$ROOT/.deps/src/brpc-${BRPC_VER}.tar.gz"
URL="https://github.com/apache/brpc/archive/refs/tags/${BRPC_VER}.tar.gz"

if [[ ! -f "$TGZ" ]]; then
  log "download brpc ${BRPC_VER}"
  curl -fsSL -o "$TGZ" "$URL"
fi

if [[ "$SKIP_CHECKSUM" -eq 0 ]]; then
  echo "${BRPC_SHA256}  ${TGZ}" | sha256sum -c -
else
  warn "checksum skipped (--skip-checksum); not for release CI"
fi

if [[ ! -d "$SRC" ]]; then
  log "extract"
  tar -xzf "$TGZ" -C "$ROOT/.deps/src"
  # archive may unpack as brpc-1.9.0
  if [[ ! -d "$SRC" ]]; then
    found="$(find "$ROOT/.deps/src" -maxdepth 1 -type d -name 'brpc-*' | head -1)"
    [[ -n "$found" ]] && SRC="$found"
  fi
fi

log "build brpc into $PREFIX (cmake)"
BUILD_BRPC_DIR="$ROOT/.deps/build-brpc"
cmake -S "$SRC" -B "$BUILD_BRPC_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DWITH_GLOG=OFF \
  -DBUILD_UNIT_TESTS=OFF \
  -DBUILD_BRPC_TOOLS=OFF
cmake --build "$BUILD_BRPC_DIR" -j"$(nproc 2>/dev/null || echo 2)"
cmake --install "$BUILD_BRPC_DIR"

# symlink into /usr/local if writable (optional)
if [[ -w /usr/local/include ]] && [[ ! -f /usr/local/include/brpc/server.h ]]; then
  log "linking headers/libs into /usr/local (writable)"
  ln -sfn "$PREFIX/include/brpc" /usr/local/include/brpc || true
  ln -sfn "$PREFIX/lib"/libbrpc* /usr/local/lib/ 2>/dev/null || true
  ln -sfn "$PREFIX/lib64"/libbrpc* /usr/local/lib64/ 2>/dev/null || true
fi

log "done. PREFIX=$PREFIX"
echo "Add to env before cmake/build:"
echo "  export CMAKE_PREFIX_PATH=\"$PREFIX:\${CMAKE_PREFIX_PATH:-}\""
echo "  export CPATH=\"$PREFIX/include:\${CPATH:-}\""
echo "  export LIBRARY_PATH=\"$PREFIX/lib:$PREFIX/lib64:\${LIBRARY_PATH:-}\""
echo "  export LD_LIBRARY_PATH=\"$PREFIX/lib:$PREFIX/lib64:\${LD_LIBRARY_PATH:-}\""
./scripts/check_deps.sh --full
