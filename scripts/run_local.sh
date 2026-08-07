#!/usr/bin/env bash
# 本地正式启动别名 → start_formal.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT/scripts/start_formal.sh" "$@"
