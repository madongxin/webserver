#!/usr/bin/env bash
# TCP 协议压测基线（不依赖 Unity）：Login 突发 + dual-gw 循环
# 用法: ./scripts/load_tcp_baseline.sh
# 环境:
#   LOAD_DURATION_SEC   默认 1800（30 分钟）
#   LOAD_CONCURRENCY    默认 32
#   LOAD_MODE           burst-login | dual-gw | mixed
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: build game_tcp_e2e_client"; exit 1; }

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
[[ -f "$RUN_DIR/pids" ]] || { echo "ERROR: start cluster first"; exit 1; }
export GAMEMESH_RUN_DIR="$RUN_DIR"
# shellcheck disable=SC1090
[[ -f "$RUN_DIR/E2E_PORTS.env" ]] && source "$RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:-8081}"
GW1="${E2E_GW1_GAME:-8083}"
DUR="${LOAD_DURATION_SEC:-1800}"
CONC="${LOAD_CONCURRENCY:-32}"
MODE="${LOAD_MODE:-mixed}"
REPORT_DIR="${LOAD_REPORT_DIR:-$ROOT/run/load}"
mkdir -p "$REPORT_DIR"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
COMMIT="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
REPORT_TXT="$REPORT_DIR/load_${STAMP}.txt"
REPORT_JSON="$REPORT_DIR/load_${STAMP}.json"
LAT_DIR="$REPORT_DIR/.lat_$$"
mkdir -p "$LAT_DIR"

sample_procs() {
  local tag="$1" out="$2"
  local rss=0 fd=0 thr=0 dead=0
  while read -r p; do
    [[ -z "$p" ]] && continue
    if [[ ! -d "/proc/$p" ]]; then
      dead=$((dead + 1))
      continue
    fi
    rss=$((rss + $(awk '/VmRSS/{print $2}' "/proc/$p/status" 2>/dev/null || echo 0)))
    fd=$((fd + $(find "/proc/$p/fd" -maxdepth 1 2>/dev/null | wc -l)))
    thr=$((thr + $(find "/proc/$p/task" -maxdepth 1 2>/dev/null | wc -l)))
  done <"$RUN_DIR/pids"
  printf '%s rss_kb=%s fd=%s threads=%s dead_pids=%s\n' "$tag" "$rss" "$fd" "$thr" "$dead" >>"$out"
  echo "$rss $fd $thr $dead"
}

echo "load: duration=${DUR}s concurrency=$CONC mode=$MODE gw0=$GW0 gw1=$GW1"
PROC_SNAP="$REPORT_DIR/load_${STAMP}.procs"
read -r RSS0 FD0 THR0 DEAD0 <<<"$(sample_procs start "$PROC_SNAP")"

ok=0
fail=0
timeout_n=0
start=$(date +%s)
end=$((start + DUR))

worker() {
  local id="$1"
  local local_ok=0 local_fail=0 local_to=0
  local latf="$LAT_DIR/w_${id}.lat"
  : >"$latf"
  while (( $(date +%s) < end )); do
    local out rc=0 kind=login t0 t1 ms
    t0=$(date +%s%N)
    if [[ "$MODE" == "burst-login" ]]; then
      kind=login
      out="$("$CLIENT" register-login "$HOST" "$GW0" "load_${id}_$$_$RANDOM" "e2epass1" 2>&1)" || rc=$?
      echo "$out" | grep -q 'login_ok=1' || rc=1
    elif [[ "$MODE" == "dual-gw" ]]; then
      kind=dual
      out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$((900000 + RANDOM % 99999))" 0 2>&1)" || rc=$?
      echo "$out" | grep -q 'dual_gw_ok=1' || rc=1
    else
      if (( RANDOM % 2 == 0 )); then
        kind=login
        out="$("$CLIENT" register-login "$HOST" "$GW0" "load_${id}_$$_$RANDOM" "e2epass1" 2>&1)" || rc=$?
        echo "$out" | grep -q 'login_ok=1' || rc=1
      else
        kind=dual
        out="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$((900000 + RANDOM % 99999))" 0 2>&1)" || rc=$?
        echo "$out" | grep -q 'dual_gw_ok=1' || rc=1
      fi
    fi
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    echo "$kind $ms $rc" >>"$latf"
    if [[ "$rc" -eq 0 ]]; then
      local_ok=$((local_ok + 1))
    else
      local_fail=$((local_fail + 1))
      if [[ "$ms" -ge 8000 ]]; then
        local_to=$((local_to + 1))
      fi
    fi
  done
  echo "$local_ok $local_fail $local_to" >"$REPORT_DIR/.w_${id}.$$"
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
for f in "$REPORT_DIR"/.w_*."$$"; do
  read -r a b c <"$f"
  ok=$((ok + a))
  fail=$((fail + b))
  timeout_n=$((timeout_n + c))
  rm -f "$f"
done
shopt -u nullglob

read -r RSS1 FD1 THR1 DEAD1 <<<"$(sample_procs end "$PROC_SNAP")"
total=$((ok + fail))
rate=0
(( total > 0 )) && rate=$((ok * 100 / total))

# 百分位 + 错误分类
export LAT_DIR REPORT_JSON REPORT_TXT STAMP COMMIT DUR CONC MODE ok fail timeout_n rate total \
  RSS0 FD0 THR0 DEAD0 RSS1 FD1 THR1 DEAD1 HOST GW0 GW1 ROOT
python3 <<'PY'
import json, os, glob, statistics

lat_login, lat_dual = [], []
err = {"login": 0, "dual": 0, "timeout_like": 0, "other": 0}
for path in glob.glob(os.path.join(os.environ["LAT_DIR"], "*.lat")):
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            kind, ms, rc = parts[0], int(parts[1]), int(parts[2])
            if rc == 0:
                (lat_login if kind == "login" else lat_dual).append(ms)
            else:
                if ms >= 8000:
                    err["timeout_like"] += 1
                if kind == "login":
                    err["login"] += 1
                elif kind == "dual":
                    err["dual"] += 1
                else:
                    err["other"] += 1

def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(xs) - 1)
    if f == c:
        return xs[f]
    return xs[f] + (xs[c] - xs[f]) * (k - f)

def summary(xs):
    return {
        "count": len(xs),
        "p50_ms": pct(xs, 50),
        "p95_ms": pct(xs, 95),
        "p99_ms": pct(xs, 99),
        "avg_ms": (sum(xs) / len(xs)) if xs else None,
    }

rep = {
    "commit": os.environ["COMMIT"],
    "stamp": os.environ["STAMP"],
    "toolchain_host": os.uname().nodename if hasattr(os, "uname") else "",
    "duration_sec": int(os.environ["DUR"]),
    "concurrency": int(os.environ["CONC"]),
    "mode": os.environ["MODE"],
    "gw0": os.environ["GW0"],
    "gw1": os.environ["GW1"],
    "total_requests": int(os.environ["total"]),
    "ok": int(os.environ["ok"]),
    "fail": int(os.environ["fail"]),
    "timeout_like": int(os.environ["timeout_n"]),
    "success_rate_pct": int(os.environ["rate"]),
    "login_latency": summary(lat_login),
    "dispatch_dual_gw_latency": summary(lat_dual),
    "error_counts": err,
    "process_rss_kb": {"start": int(os.environ["RSS0"]), "end": int(os.environ["RSS1"])},
    "process_fd": {"start": int(os.environ["FD0"]), "end": int(os.environ["FD1"])},
    "process_threads": {"start": int(os.environ["THR0"]), "end": int(os.environ["THR1"])},
    "process_dead_pids": {"start": int(os.environ["DEAD0"]), "end": int(os.environ["DEAD1"])},
    "notes": {
        "player_serial_queue": "see OpsMetrics / logs if instrumented",
        "brpc_timeouts": "approximated as client ops >= 8000ms",
        "redis_mysql_latency": "not sampled in this baseline; see service logs",
    },
}
with open(os.environ["REPORT_JSON"], "w", encoding="utf-8") as f:
    json.dump(rep, f, indent=2, ensure_ascii=False)
    f.write("\n")

lines = [
    f"stamp={rep['stamp']}",
    f"commit={rep['commit']}",
    f"duration_sec={rep['duration_sec']}",
    f"concurrency={rep['concurrency']}",
    f"mode={rep['mode']}",
    f"ok={rep['ok']}",
    f"fail={rep['fail']}",
    f"timeout_like={rep['timeout_like']}",
    f"success_rate_pct={rep['success_rate_pct']}",
    f"login_p50_ms={rep['login_latency']['p50_ms']}",
    f"login_p95_ms={rep['login_latency']['p95_ms']}",
    f"login_p99_ms={rep['login_latency']['p99_ms']}",
    f"dual_p50_ms={rep['dispatch_dual_gw_latency']['p50_ms']}",
    f"dual_p95_ms={rep['dispatch_dual_gw_latency']['p95_ms']}",
    f"dual_p99_ms={rep['dispatch_dual_gw_latency']['p99_ms']}",
    f"rss_kb_start={rep['process_rss_kb']['start']}",
    f"rss_kb_end={rep['process_rss_kb']['end']}",
    f"fd_start={rep['process_fd']['start']}",
    f"fd_end={rep['process_fd']['end']}",
    f"threads_start={rep['process_threads']['start']}",
    f"threads_end={rep['process_threads']['end']}",
    f"dead_pids_end={rep['process_dead_pids']['end']}",
]
with open(os.environ["REPORT_TXT"], "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print("\n".join(lines))
PY

rm -rf "$LAT_DIR"

MIN_RATE="${LOAD_MIN_SUCCESS_PCT:-95}"
if (( rate < MIN_RATE )); then
  echo "ERROR: success_rate $rate% < $MIN_RATE%" >&2
  exit 1
fi
if (( DEAD1 > 0 )); then
  echo "ERROR: process exits detected dead_pids_end=$DEAD1" >&2
  exit 1
fi
echo "load_tcp_baseline.sh PASS report=$REPORT_JSON"
