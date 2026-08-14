#!/usr/bin/env bash
# Soak：持续业务流量 + 采样 RSS/FD/线程；默认 2 小时
# 用法:
#   ./scripts/soak_test.sh
#   SOAK_DURATION_SEC=120 ./scripts/soak_test.sh
#   SOAK_REPORT=path ./scripts/soak_test.sh --verify-only
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
  if [[ "$R" == *.json ]]; then
    python3 - <<PY
import json,sys
m=json.load(open("$R"))
assert m.get("commit")=="$COMMIT", "commit mismatch"
assert m.get("result")=="PASS", "not PASS"
print("soak_test.sh PASS (verified json $R)")
PY
  else
    grep -q "commit=$COMMIT" "$R" || { echo "ERROR: report commit mismatch"; exit 1; }
    grep -q 'result=PASS' "$R" || { echo "ERROR: report not PASS"; exit 1; }
    echo "soak_test.sh PASS (verified $R)"
  fi
  exit 0
fi

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: build client"; exit 1; }
RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
[[ -f "$RUN_DIR/pids" ]] || { echo "ERROR: start cluster"; exit 1; }
export GAMEMESH_RUN_DIR="$RUN_DIR"
# shellcheck disable=SC1090
[[ -f "$RUN_DIR/E2E_PORTS.env" ]] && source "$RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:-8081}"
GW1="${E2E_GW1_GAME:-8083}"
DUR="${SOAK_DURATION_SEC:-7200}"
SAMPLE="${SOAK_SAMPLE_SEC:-30}"
STAMP="$(date -u +%Y%m%dT%H:%M:%SZ)"
# fix stamp format (no colons in filename)
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
REPORT_TXT="$REPORT_DIR/soak_${STAMP}.txt"
REPORT_JSON="$REPORT_DIR/soak_${STAMP}.json"
SAMPLE_LOG="$REPORT_DIR/soak_${STAMP}.samples"

echo "soak: duration=${DUR}s sample=${SAMPLE}s report=$REPORT_JSON"
# 负载后复用集群可能进程仍活但 login 已卡死；先探测，避免空跑 2 小时
CLIENT_WARM="$CLIENT"
warm=0
last_warm=""
for _ in $(seq 1 30); do
  last_warm="$("$CLIENT_WARM" register-login "$HOST" "$GW0" "soak_warm_$$_$_" e2epass1 2>&1 || true)"
  if echo "$last_warm" | grep -q login_ok=1; then
    warm=1
    break
  fi
  sleep 1
done
if [[ "$warm" -ne 1 ]]; then
  echo "ERROR: soak cluster not accepting register-login within 30s" >&2
  echo "$last_warm" >&2
  exit 1
fi
ok=0
fail=0
timeout_n=0
proc_exits=0
start=$(date +%s)
end=$((start + DUR))
rss0=0
fd0=0
thr0=0

sample_once() {
  # 在父 shell 更新 rss0/fd0/thr0/proc_exits（禁止命令替换子 shell）
  local rss=0 fd=0 thr=0 dead=0
  if [[ -f "$RUN_DIR/pids" ]]; then
    while read -r p; do
      [[ -n "$p" ]] || continue
      if [[ ! -d "/proc/$p" ]]; then
        dead=$((dead + 1))
        continue
      fi
      rss=$((rss + $(awk '/VmRSS/{print $2}' "/proc/$p/status" 2>/dev/null || echo 0)))
      fd=$((fd + $(find "/proc/$p/fd" -maxdepth 1 2>/dev/null | wc -l)))
      thr=$((thr + $(find "/proc/$p/task" -maxdepth 1 2>/dev/null | wc -l)))
    done <"$RUN_DIR/pids"
  fi
  echo "$(date -u +%H:%M:%S) rss_kb=$rss fd=$fd threads=$thr ok=$ok fail=$fail dead=$dead" >>"$SAMPLE_LOG"
  if [[ "$rss0" -eq 0 ]]; then
    rss0=$rss
    fd0=$fd
    thr0=$thr
  fi
  rss_end=$rss
  fd_end=$fd
  thr_end=$thr
  if (( dead > 0 )); then
    proc_exits=$((proc_exits + dead))
    echo "ERROR: process exit detected dead=$dead" >&2
    return 1
  fi
  if (( rss0 > 0 && rss > rss0 * 8 && rss > 2000000 )); then
    echo "ERROR: RSS growth suspicious rss0=$rss0 rss=$rss" >&2
    return 1
  fi
  if (( DUR >= 1800 && rss0 > 0 && rss > rss0 * 3 && fd > fd0 * 2 && thr > thr0 * 2 && rss > 500000 )); then
    echo "ERROR: sustained growth rss=$rss/$rss0 fd=$fd/$fd0 thr=$thr/$thr0" >&2
    return 1
  fi
  return 0
}

next_sample=$start
rss_end=0
fd_end=0
thr_end=0
sample_once || exit 1
while (( $(date +%s) < end )); do
  now=$(date +%s)
  if (( now >= next_sample )); then
    sample_once || exit 1
    next_sample=$((now + SAMPLE))
  fi
  t0=$(date +%s%N)
  if out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$((910000 + RANDOM % 89999))" 0 2>&1)"; then
    if echo "$out" | grep -q 'dual_gw_ok=1'; then
      ok=$((ok + 1))
    else
      fail=$((fail + 1))
    fi
  else
    fail=$((fail + 1))
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    (( ms >= 8000 )) && timeout_n=$((timeout_n + 1))
  fi
done
sample_once || exit 1

total=$((ok + fail))
rate=0
(( total > 0 )) && rate=$((ok * 100 / total))
MIN_RATE="${SOAK_MIN_SUCCESS_PCT:-90}"
result=PASS
if (( rate < MIN_RATE )); then
  result=FAIL
fi
if (( proc_exits > 0 )); then
  result=FAIL
fi

export REPORT_TXT REPORT_JSON SAMPLE_LOG STAMP COMMIT DUR ok fail timeout_n rate total result \
  rss0 fd0 thr0 rss_end fd_end thr_end proc_exits
python3 <<'PY'
import json, os
rep = {
  "commit": os.environ["COMMIT"],
  "stamp": os.environ["STAMP"],
  "duration_sec": int(os.environ["DUR"]),
  "ok": int(os.environ["ok"]),
  "fail": int(os.environ["fail"]),
  "timeout_like": int(os.environ["timeout_n"]),
  "success_rate_pct": int(os.environ["rate"]),
  "total_requests": int(os.environ["total"]),
  "result": os.environ["result"],
  "rss_kb": {"start": int(os.environ["rss0"]), "end": int(os.environ["rss_end"])},
  "fd": {"start": int(os.environ["fd0"]), "end": int(os.environ["fd_end"])},
  "threads": {"start": int(os.environ["thr0"]), "end": int(os.environ["thr_end"])},
  "process_abnormal_exits": int(os.environ["proc_exits"]),
  "samples": os.environ["SAMPLE_LOG"],
  "notes": {
    "gateway_connections": "sample via /proc and logs",
    "player_serial_queue": "see OpsMetrics if enabled",
    "brpc_timeouts": "client ops >=8000ms counted as timeout_like",
    "redis_mysql": "shared infra; errors appear in service logs",
  },
}
with open(os.environ["REPORT_JSON"], "w", encoding="utf-8") as f:
    json.dump(rep, f, indent=2, ensure_ascii=False)
    f.write("\n")
txt = "\n".join([
    f"stamp={rep['stamp']}",
    f"commit={rep['commit']}",
    f"duration_sec={rep['duration_sec']}",
    f"ok={rep['ok']}",
    f"fail={rep['fail']}",
    f"timeout_like={rep['timeout_like']}",
    f"success_rate_pct={rep['success_rate_pct']}",
    f"rss0_kb={rep['rss_kb']['start']}",
    f"rss_end_kb={rep['rss_kb']['end']}",
    f"fd0={rep['fd']['start']}",
    f"fd_end={rep['fd']['end']}",
    f"threads0={rep['threads']['start']}",
    f"threads_end={rep['threads']['end']}",
    f"process_abnormal_exits={rep['process_abnormal_exits']}",
    f"samples={rep['samples']}",
    f"result={rep['result']}",
]) + "\n"
open(os.environ["REPORT_TXT"], "w", encoding="utf-8").write(txt)
print(txt, end="")
PY

[[ "$result" == "PASS" ]] || { echo "soak_test.sh FAIL"; exit 1; }
echo "soak_test.sh PASS report=$REPORT_JSON"
echo "$REPORT_JSON" >"$REPORT_DIR/LATEST_PASS.txt"
