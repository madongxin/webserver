#!/usr/bin/env bash
# 检查构建依赖（不自动安装；不 sudo）
# 用法：
#   ./scripts/check_deps.sh           # 基础依赖
#   ./scripts/check_deps.sh --full    # 要求 brpc（完整分布式构建）
#   ./scripts/check_deps.sh --lowlevel
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODE="base"
for arg in "$@"; do
  case "$arg" in
    --full) MODE="full" ;;
    --lowlevel) MODE="lowlevel" ;;
    -h|--help)
      echo "usage: $0 [--full|--lowlevel]"
      exit 0
      ;;
  esac
done

fail=0
ok() { printf '[OK] %s\n' "$*"; }
bad() { printf '[MISSING] %s\n' "$*"; fail=1; }
warn() { printf '[WARN] %s\n' "$*"; }

echo "== GameMesh dependency check (root=$ROOT mode=$MODE) =="

command -v g++ >/dev/null && ok "g++ $(g++ --version | head -1)" || bad "g++"
command -v cmake >/dev/null && ok "cmake $(cmake --version | head -1)" || bad "cmake"
command -v protoc >/dev/null && ok "protoc $(protoc --version)" || bad "protoc"

if [[ -f /usr/include/json/json.h ]] || [[ -f /usr/include/jsoncpp/json/json.h ]]; then
  ok "jsoncpp headers"
else
  bad "jsoncpp headers (json/json.h)"
fi

if [[ -f /usr/include/openssl/evp.h ]]; then
  ok "OpenSSL headers"
else
  bad "OpenSSL headers (openssl/evp.h)"
fi

if [[ "$MODE" != "lowlevel" ]]; then
  if [[ -f /usr/include/mysql/mysql.h ]] || [[ -f /usr/include/mysql.h ]]; then
    ok "MySQL client headers"
  else
    bad "MySQL client headers (needed if ENABLE_MYSQL=ON)"
  fi
  if [[ -f /usr/include/hiredis/hiredis.h ]]; then
    ok "hiredis headers"
  else
    bad "hiredis headers (needed if ENABLE_REDIS=ON)"
  fi
fi

has_brpc=0
if [[ -f /usr/local/include/brpc/server.h ]] || [[ -f /usr/include/brpc/server.h ]]; then
  has_brpc=1
  ok "brpc headers"
  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists brpc 2>/dev/null; then
    ok "brpc pkg-config $(pkg-config --modversion brpc 2>/dev/null || echo unknown)"
  fi
else
  if [[ "$MODE" == "full" ]]; then
    bad "brpc headers (required for --full / ENABLE_BRPC=ON)"
  else
    warn "brpc headers not found (required for ENABLE_BRPC=ON full build)"
  fi
fi

if [[ -f "$ROOT/game/game.pb.cc" ]]; then
  ok "game/game.pb.cc present"
else
  bad "game/game.pb.cc (run: protoc -I proto --cpp_out=game proto/game.proto)"
fi

# sanitizer 运行时（缺失时仍可普通构建；TSan 链接会失败）
if [[ -e /usr/lib64/libasan.so.6 || -e /usr/lib/libasan.so.6 ]]; then
  ok "libasan runtime"
else
  warn "libasan runtime missing (needed for ENABLE_ASAN)"
fi
if [[ -e /usr/lib64/libubsan.so.1 || -e /usr/lib/libubsan.so.1 ]]; then
  ok "libubsan runtime"
else
  warn "libubsan runtime missing (needed for ENABLE_UBSAN)"
fi
if [[ -e /usr/lib64/libtsan.so.0 || -e /usr/lib/libtsan.so.0 ]]; then
  ok "libtsan runtime"
else
  warn "libtsan runtime missing (needed for ENABLE_TSAN; yum install libtsan)"
fi
if command -v shellcheck >/dev/null 2>&1 || command -v ShellCheck >/dev/null 2>&1; then
  ok "shellcheck"
else
  warn "shellcheck missing (stable_gate --full requires it)"
fi

if [[ -f "$ROOT/config/mysql.cnf" ]]; then
  ok "config/mysql.cnf present"
else
  warn "config/mysql.cnf missing (copy from mysql.cnf.example)"
fi

# protobuf / protoc 粗兼容提示（不强制锁死小版本）
if command -v protoc >/dev/null 2>&1; then
  pv="$(protoc --version | awk '{print $2}')"
  ok "protoc version=$pv (generated *.pb.cc must match this toolchain)"
fi

if [[ "$fail" -ne 0 ]]; then
  echo "== FAILED: fix MISSING items before build =="
  exit 1
fi
echo "== ALL REQUIRED CHECKS PASSED (mode=$MODE brpc=$has_brpc) =="
