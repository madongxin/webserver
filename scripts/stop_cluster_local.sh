#!/usr/bin/env bash
# 停止本地集群：优先 PID 文件；可选 docker compose down
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODE="${1:-auto}"

stop_pids() {
  local run_dir="${GAMEMESH_RUN_DIR:-$ROOT/run/cluster}"
  if [[ -f "$run_dir/pids" ]]; then
    GAMEMESH_RUN_DIR="$run_dir" "$ROOT/scripts/stop_cluster.sh"
    return 0
  fi
  run_dir="$ROOT/run/formal"
  if [[ -f "$run_dir/pids" ]]; then
    GAMEMESH_RUN_DIR="$run_dir" "$ROOT/scripts/stop_cluster.sh"
    return 0
  fi
  echo "no local pids under run/cluster or run/formal"
  return 1
}

stop_compose() {
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker not available" >&2
    return 1
  fi
  docker compose -f "$ROOT/compose.yml" down --remove-orphans
}

case "$MODE" in
  pids)
    stop_pids
    ;;
  compose|docker)
    stop_compose
    ;;
  auto)
    if stop_pids 2>/dev/null; then
      :
    elif docker compose -f "$ROOT/compose.yml" ps -q 2>/dev/null | grep -q .; then
      stop_compose
    else
      echo "nothing to stop"
    fi
    ;;
  *)
    echo "usage: $0 [auto|pids|compose]" >&2
    exit 2
    ;;
esac
echo "stop_cluster_local.sh done"
