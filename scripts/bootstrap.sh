#!/usr/bin/env bash
# 阶段 3：依赖检查 + 安装指引（不自动改系统包）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT/scripts/check_deps.sh"
