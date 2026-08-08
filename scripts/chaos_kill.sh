#!/usr/bin/env bash
# 受控 kill 某一角色后观察（默认 gamelogic0）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/formal}"
TARGET="${1:-logic0}"
LOG="$RUN_DIR/logs/${TARGET}.log"
if [[ ! -f "$RUN_DIR/pids" ]]; then
  echo "cluster not running"
  exit 1
fi
# 按日志进程名启发式：取 pids 中仍存活的一个
mapfile -t PIDS <"$RUN_DIR/pids"
victim=""
for p in "${PIDS[@]}"; do
  if [[ -n "$p" ]] && kill -0 "$p" 2>/dev/null; then
    cmdline=$(tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null || true)
    if [[ "$cmdline" == *"$TARGET"* ]] || [[ "$cmdline" == *gamelogic* && "$TARGET" == logic0 ]]; then
      victim="$p"
      break
    fi
  fi
done
if [[ -z "$victim" ]]; then
  # 回退：杀倒数第 3 个 pid（formal 启动顺序近似）
  victim="${PIDS[$(( ${#PIDS[@]} > 3 ? ${#PIDS[@]} - 4 : 0 ))]}"
fi
echo "chaos kill pid=$victim target=$TARGET"
kill -KILL "$victim" 2>/dev/null || true
echo "see $LOG and scripts/kill_logic_and_recover.sh for placement recover"
