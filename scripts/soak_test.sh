#!/usr/bin/env bash
# Soak：持续业务流量 + 采样 RSS/FD/队列；默认 2 小时
# 用法:
#   ./scripts/soak_test.sh
#   SOAK_DURATION_SEC=120 ./scripts/soak_test.sh   # smoke
#   SOAK_REPORT=path ./scripts/soak_test.sh --verify-only  # 校验已有报告（同 commit）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERIFY_ONLY=0
[[ "${1:-}" == "--verify-only" ]] && VERIFY_ONLY=1

REPORT_DIR="${SOAK_REPORT_DIR:-$ROOT/run/soak}"
mkdir -p "$REPORT_DIR"
COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"

if [[ "$VERIFY_ONLY" -eq 1 ]]; then
  R="${SOAK_REPORT:-}"
  [[ -n "$R" && -f "$R" ]] || { echo "ERROR: set SOAK_REPORT to existing report"; exit 1; }
  grep -q "commit=$COMMIT" "$R" || { echo "ERROR: report commit mismatch"; exit 1; }
  grep -q 'result=PASS' "$R" || { echo "ERROR: report not PASS"; exit 1; }
  echo "soak_test.sh PASS (verified $R)"
  exit 0
fi

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: build client"; exit 1; }
RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
[[ -f "$RUN_DIR/pids" ]] || { echo "ERROR: start cluster"; exit 1; }
export GAMEMESH_RUN_DIR="$RUN_DIR"
if [[ -f "$RUN_DIR/E2E_PORTS.env" ]]; then
  # shellcheck disable=SC1090
  source "$RUN_DIR/E2E_PORTS.env"
fi

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:-8081}"
GW1="${E2E_GW1_GAME:-8083}"
DUR="${SOAK_DURATION_SEC:-7200}"
SAMPLE="${SOAK_SAMPLE_SEC:-30}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
REPORT="$REPORT_DIR/soak_${STAMP}.txt"
SAMPLE_LOG="$REPORT_DIR/soak_${STAMP}.samples"

echo "soak: duration=${DUR}s sample=${SAMPLE}s report=$REPORT"
ok=0
fail=0
start=$(date +%s)
end=$((start + DUR))
rss0=0

sample_once() {
  local pids_file="$RUN_DIR/pids"
  local rss=0 fd=0 thr=0
  if [[ -f "$pids_file" ]]; then
    while read -r p; do
      [[ -n "$p" && -d "/proc/$p" ]] || continue
      rss=$((rss + $(awk '/VmRSS/{print $2}' "/proc/$p/status" 2>/dev/null || echo 0)))
      fd=$((fd + $(find "/proc/$p/fd" -maxdepth 1 2>/dev/null | wc -l)))
      thr=$((thr + $(find "/proc/$p/task" -maxdepth 1 2>/dev/null | wc -l)))
    done <"$pids_file"
  fi
  echo "$(date -u +%H:%M:%S) rss_kb=$rss fd=$fd threads=$thr ok=$ok fail=$fail" >>"$SAMPLE_LOG"
  if [[ "$rss0" -eq 0 ]]; then
    rss0=$rss
  fi
  # 粗泄漏阈值：RSS 增长 > 8x 且绝对值 > 2GB 视为失败信号
  if (( rss > rss0 * 8 && rss > 2000000 )); then
    echo "ERROR: RSS growth suspicious rss0=$rss0 rss=$rss" >&2
    return 1
  fi
  return 0
}

next_sample=$start
while (( $(date +%s) < end )); do
  now=$(date +%s)
  if (( now >= next_sample )); then
    sample_once || exit 1
    next_sample=$((now + SAMPLE))
  fi
  if out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$((910000 + RANDOM % 89999))" 0 2>&1)"; then
    if echo "$out" | grep -q 'dual_gw_ok=1'; then
      ok=$((ok + 1))
    else
      fail=$((fail + 1))
    fi
  else
    fail=$((fail + 1))
  fi
done
sample_once || true

total=$((ok + fail))
rate=0
(( total > 0 )) && rate=$((ok * 100 / total))
MIN_RATE="${SOAK_MIN_SUCCESS_PCT:-90}"
result=PASS
if (( rate < MIN_RATE )); then
  result=FAIL
fi

{
  echo "stamp=$STAMP"
  echo "commit=$COMMIT"
  echo "duration_sec=$DUR"
  echo "ok=$ok"
  echo "fail=$fail"
  echo "success_rate_pct=$rate"
  echo "rss0_kb=$rss0"
  echo "samples=$SAMPLE_LOG"
  echo "result=$result"
} | tee "$REPORT"

[[ "$result" == "PASS" ]] || { echo "soak_test.sh FAIL"; exit 1; }
echo "soak_test.sh PASS report=$REPORT"
# 方便门禁引用
echo "$REPORT" >"$REPORT_DIR/LATEST_PASS.txt"
