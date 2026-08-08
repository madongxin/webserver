#!/usr/bin/env bash
# 优雅停止：先 SIGTERM，等待 readiness/进程退出，再 SIGKILL
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/formal}"
if [[ ! -f "$RUN_DIR/pids" ]]; then
  echo "no pids at $RUN_DIR/pids"
  exit 0
fi
mapfile -t PIDS <"$RUN_DIR/pids"
echo "SIGTERM ${#PIDS[@]} processes..."
for p in "${PIDS[@]}"; do
  [[ -n "$p" ]] && kill -TERM "$p" 2>/dev/null || true
done
for i in $(seq 1 40); do
  alive=0
  for p in "${PIDS[@]}"; do
    if [[ -n "$p" ]] && kill -0 "$p" 2>/dev/null; then
      alive=1
    fi
  done
  [[ "$alive" -eq 0 ]] && break
  sleep 0.25
done
for p in "${PIDS[@]}"; do
  if [[ -n "$p" ]] && kill -0 "$p" 2>/dev/null; then
    echo "SIGKILL $p"
    kill -KILL "$p" 2>/dev/null || true
  fi
done
rm -f "$RUN_DIR/pids"
echo "stop_cluster.sh done"
