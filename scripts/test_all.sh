#!/usr/bin/env bash
# 串行执行已有测试入口；任一步失败则整体失败
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== test_all: reactor =="
"${ROOT}/scripts/test_reactor.sh"
echo "== test_all: unit =="
"${ROOT}/scripts/test_unit.sh"
echo "== test_all: integration =="
"${ROOT}/scripts/test_integration.sh"
echo "test_all.sh PASS"
