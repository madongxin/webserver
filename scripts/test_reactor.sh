#!/usr/bin/env bash
# Reactor / ProtoFraming 单元 + 集成测试
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BIN="${ROOT}/build/test"

for name in reactor_unit_test reactor_integration_test; do
  if [[ ! -x "$BIN/$name" ]]; then
    echo "ERROR missing binary: $BIN/$name (build first)" >&2
    exit 1
  fi
  echo "== $name =="
  "$BIN/$name"
done
echo "test_reactor.sh PASS"
