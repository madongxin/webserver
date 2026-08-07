#!/usr/bin/env bash
# 从 proto/ 生成提交用的 *.pb.cc / *.pb.h（CMake 不自动跑 protoc）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v protoc >/dev/null 2>&1; then
  echo "protoc not found" >&2
  exit 1
fi

protoc -I proto --cpp_out=game proto/game.proto
protoc -I proto --cpp_out=game/brpc proto/mail_brpc.proto
protoc -I proto --cpp_out=rocksdb proto/kv_demo.proto
protoc -I proto --cpp_out=runtime/brpc \
  proto/forward.proto \
  proto/session.proto \
  proto/auth.proto \
  proto/gamelogic_rpc.proto \
  proto/gateway_push.proto \
  proto/gamedb.proto

echo "generated pb sources under game/, game/brpc/, rocksdb/, runtime/brpc/"
