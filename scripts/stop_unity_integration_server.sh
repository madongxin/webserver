#!/usr/bin/env bash
# 仅按 PID 文件停止 Unity 联调集群，不按进程名误杀。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/unity-e2e}"
exec "$ROOT/scripts/stop_e2e_cluster.sh"
