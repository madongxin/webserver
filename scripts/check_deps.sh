#!/usr/bin/env bash
# 检查阶段 0 基线构建所需依赖（不自动安装系统包）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

fail=0
ok() { printf '[OK] %s\n' "$*"; }
bad() { printf '[MISSING] %s\n' "$*"; fail=1; }

echo "== GameMesh dependency check (root=$ROOT) =="

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

if [[ -f /usr/local/include/brpc/server.h ]] || [[ -f /usr/include/brpc/server.h ]]; then
  ok "brpc headers"
else
  printf '[WARN] brpc headers not found (only needed if ENABLE_BRPC=ON)\n'
fi

if [[ -f "$ROOT/game/game.pb.cc" ]]; then
  ok "game/game.pb.cc present"
else
  bad "game/game.pb.cc (run: protoc -I proto --cpp_out=game proto/game.proto)"
fi

if [[ -f "$ROOT/config/mysql.cnf" ]]; then
  ok "config/mysql.cnf present"
else
  printf '[WARN] config/mysql.cnf missing (copy from mysql.cnf.example)\n'
fi

if [[ "$fail" -ne 0 ]]; then
  echo "== FAILED: fix MISSING items before build =="
  exit 1
fi
echo "== ALL REQUIRED CHECKS PASSED =="
