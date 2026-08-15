#!/usr/bin/env bash
# Unity 联调专用集群：GW×2 Session/Auth×2 GameLogic×2 World GameDB×2
# 默认 run/unity-e2e，避免与 run/e2e 冲突。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/unity-e2e}"
export GAMEMESH_FORMAL="${GAMEMESH_FORMAL:-1}"
exec "$ROOT/scripts/run_e2e_cluster.sh"
