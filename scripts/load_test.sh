#!/usr/bin/env bash
# 轻量负载：对 HTTP liveness 发 N 次请求（游戏口负载用专用客户端）
set -euo pipefail
HOST="${1:-127.0.0.1}"
PORT="${2:-8080}"
N="${3:-100}"
RATE="${4:-50}"
ok=0
for i in $(seq 1 "$N"); do
  if curl -fsS -m 1 "http://${HOST}:${PORT}/api/liveness" >/dev/null; then
    ok=$((ok + 1))
  fi
  # 粗略限速
  if (( i % RATE == 0 )); then
    sleep 1
  fi
done
echo "load_test.sh ok=$ok/$N"
[[ "$ok" -eq "$N" ]]
