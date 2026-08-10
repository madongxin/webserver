#!/usr/bin/env bash
# TCP 协议压测基线（不依赖 Unity）：Login 突发 + dual-gw 循环
# 用法:
#   ./scripts/load_tcp_baseline.sh
# 环境:
#   LOAD_DURATION_SEC   默认 1800（30 分钟）；smoke 可设 60
#   LOAD_CONCURRENCY    并行客户端数，默认 32（可升到 1000 需机器资源）
#   LOAD_MODE           burst-login | dual-gw | mixed（默认 mixed）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: build game_tcp_e2e_client"; exit 1; }

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
[[ -f "$RUN_DIR/pids" ]] || { echo "ERROR: start cluster first"; exit 1; }
export GAMEMESH_RUN_DIR="$RUN_DIR"
if [[ -f "$RUN_DIR/E2E_PORTS.env" ]]; then
  # shellcheck disable=SC1090
  source "$RUN_DIR/E2E_PORTS.env"
fi

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:-8081}"
GW1="${E2E_GW1_GAME:-8083}"
DUR="${LOAD_DURATION_SEC:-1800}"
CONC="${LOAD_CONCURRENCY:-32}"
MODE="${LOAD_MODE:-mixed}"
REPORT_DIR="${LOAD_REPORT_DIR:-$ROOT/run/load}"
mkdir -p "$REPORT_DIR"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
REPORT="$REPORT_DIR/load_${STAMP}.txt"

echo "load: duration=${DUR}s concurrency=$CONC mode=$MODE gw0=$GW0 gw1=$GW1"
ok=0
fail=0
start=$(date +%s)
end=$((start + DUR))

worker() {
  local id="$1"
  local local_ok=0 local_fail=0
  while (( $(date +%s) < end )); do
    local out rc=0
    if [[ "$MODE" == "burst-login" ]]; then
      out="$("$CLIENT" register-login "$HOST" "$GW0" "load_${id}_$$_$RANDOM" "e2epass1" 2>&1)" || rc=$?
      echo "$out" | grep -q 'login_ok=1' || rc=1
    elif [[ "$MODE" == "dual-gw" ]]; then
      out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$((900000 + RANDOM % 99999))" 0 2>&1)" || rc=$?
      echo "$out" | grep -q 'dual_gw_ok=1' || rc=1
    else
      if (( RANDOM % 2 == 0 )); then
        out="$("$CLIENT" register-login "$HOST" "$GW0" "load_${id}_$$_$RANDOM" "e2epass1" 2>&1)" || rc=$?
        echo "$out" | grep -q 'login_ok=1' || rc=1
      else
        out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$((900000 + RANDOM % 99999))" 0 2>&1)" || rc=$?
        echo "$out" | grep -q 'dual_gw_ok=1' || rc=1
      fi
    fi
    if [[ "$rc" -eq 0 ]]; then
      local_ok=$((local_ok + 1))
    else
      local_fail=$((local_fail + 1))
    fi
  done
  echo "$local_ok $local_fail" >"$REPORT_DIR/.w_${id}.$$"
}

pids=()
for i in $(seq 1 "$CONC"); do
  worker "$i" &
  pids+=($!)
done
for p in "${pids[@]}"; do
  wait "$p" || true
done

shopt -s nullglob
# shellcheck disable=SC2231
for f in "$REPORT_DIR"/.w_*."$$"; do
  read -r a b <"$f"
  ok=$((ok + a))
  fail=$((fail + b))
  rm -f "$f"
done
shopt -u nullglob
total=$((ok + fail))
rate=0
(( total > 0 )) && rate=$((ok * 100 / total))

{
  echo "stamp=$STAMP"
  echo "duration_sec=$DUR"
  echo "concurrency=$CONC"
  echo "mode=$MODE"
  echo "ok=$ok"
  echo "fail=$fail"
  echo "success_rate_pct=$rate"
  echo "commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
} | tee "$REPORT"

# 成功率阈值：默认 ≥95%
MIN_RATE="${LOAD_MIN_SUCCESS_PCT:-95}"
if (( rate < MIN_RATE )); then
  echo "ERROR: success_rate $rate% < $MIN_RATE%" >&2
  exit 1
fi
echo "load_tcp_baseline.sh PASS report=$REPORT"
