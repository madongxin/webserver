#!/usr/bin/env bash
# 阶段 7：HTTP 探活压测，输出吞吐与延迟分位（游戏口压测另需专用客户端）
set -euo pipefail
HOST="${1:-127.0.0.1}"
PORT="${2:-8080}"
N="${3:-200}"
PATH_URL="${4:-/health/live}"
URL="http://${HOST}:${PORT}${PATH_URL}"

ok=0
fail=0
# 毫秒延迟列表
declare -a LAT_MS=()
START_NS="$(date +%s%N)"
for i in $(seq 1 "$N"); do
  t0="$(date +%s%N)"
  if curl -fsS -m 2 "$URL" >/dev/null 2>&1; then
    ok=$((ok + 1))
    t1="$(date +%s%N)"
    LAT_MS+=($(( (t1 - t0) / 1000000 )))
  else
    fail=$((fail + 1))
  fi
done
END_NS="$(date +%s%N)"
ELAPSED_MS=$(( (END_NS - START_NS) / 1000000 ))
if (( ELAPSED_MS < 1 )); then ELAPSED_MS=1; fi
QPS=$(( ok * 1000 / ELAPSED_MS ))

percentile() {
  local p="$1"
  local n=${#LAT_MS[@]}
  if (( n == 0 )); then echo 0; return; fi
  local sorted
  mapfile -t sorted < <(printf '%s\n' "${LAT_MS[@]}" | sort -n)
  local idx=$(( (p * n + 99) / 100 ))
  if (( idx < 1 )); then idx=1; fi
  if (( idx > n )); then idx=$n; fi
  echo "${sorted[$((idx - 1))]}"
}

P50="$(percentile 50)"
P95="$(percentile 95)"
P99="$(percentile 99)"
ERR_RATE="$(awk -v f="$fail" -v n="$N" 'BEGIN{printf "%.2f", (n>0)?(100.0*f/n):0}')"

echo "load_test.sh url=$URL n=$N ok=$ok fail=$fail err_rate=${ERR_RATE}% elapsed_ms=$ELAPSED_MS qps~$QPS p50_ms=$P50 p95_ms=$P95 p99_ms=$P99"
[[ "$ok" -eq "$N" ]]
