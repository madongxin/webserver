#!/usr/bin/env bash
# 稳定版本门禁入口：委托 test_final_e2e.sh（缺依赖必须失败）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT/scripts/test_final_e2e.sh" "$@"
