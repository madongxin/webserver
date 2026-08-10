#!/usr/bin/env bash
# 阶段二：GameDB 未知结果 + 进程故障后的真实资产业务
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

TOOL="${ROOT}/build/test/gamedb_rpc_tool"
[[ -x "$ROOT/build/test/gamedb_unknown_result_test" ]] || {
  echo "ERROR: build gamedb_unknown_result_test"; exit 1
}
"$ROOT/build/test/gamedb_unknown_result_test"
"$ROOT/build/test/gamedb_snapshot_idempotency_test"
[[ -x "$ROOT/build/test/gamedb_mutation_idempotency_test" ]] && \
  "$ROOT/build/test/gamedb_mutation_idempotency_test"

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"

[[ -x "$TOOL" ]] || { echo "ERROR: build gamedb_rpc_tool"; exit 1; }

PID_DB0="$(e2e_pid_of gamedb gamedb-0)" || { echo "ERROR: gamedb-0 missing"; exit 1; }
RPC_DB0="$(e2e_rpc_of gamedb gamedb-0)"
RPC_DB1="$(e2e_rpc_of gamedb gamedb-1)"
HTTP_DB0="$(e2e_http_of gamedb gamedb-0)"
HTTP_DB1="$(e2e_http_of gamedb gamedb-1)"
HOST="${E2E_HOST:-127.0.0.1}"

GAMEDB_BIN="$ROOT/build/test/gamedb"
[[ -x "$GAMEDB_BIN" ]] || { echo "ERROR: missing gamedb binary"; exit 1; }

FP_PID=""
cleanup_fp() {
  if [[ -n "${FP_PID:-}" ]] && kill -0 "$FP_PID" 2>/dev/null; then
    kill -9 "$FP_PID" 2>/dev/null || true
  fi
}
trap cleanup_fp EXIT

echo "== restart gamedb-0 with abort-after-commit failpoint =="
kill -9 "$PID_DB0" 2>/dev/null || true
sleep 1
http_port="$HTTP_DB0"
rpc_port="${RPC_DB0##*:}"
GAMEMESH_INSTANCE_ID=gamedb-0 \
  GAMEMESH_FAILPOINT_DELAY_MS=2500 \
  GAMEMESH_FAILPOINT_ABORT_AFTER_COMMIT=1 \
  nohup "$GAMEDB_BIN" "$http_port" "$rpc_port" >"$GAMEMESH_RUN_DIR/logs/gamedb0_fp.log" 2>&1 &
FP_PID=$!
echo "$FP_PID" >>"$GAMEMESH_RUN_DIR/pids"
e2e_inv_replace gamedb gamedb-0 "$FP_PID" "$RPC_DB0" "$HTTP_DB0" -
# wait listen
for _ in $(seq 1 40); do
  if grep -qE 'GameDbBrpcServer listening|role=gamedb' "$GAMEMESH_RUN_DIR/logs/gamedb0_fp.log" 2>/dev/null; then
    break
  fi
  sleep 0.25
done
curl -fsS -m 3 "http://${HOST}:${HTTP_DB0}/health/live" >/dev/null

player="$((940000000 + RANDOM % 100000))"
key="gdb_fail_$$_$RANDOM"
echo "== mutate via gamedb-0 (expect abort after commit) player=$player =="
set +e
"$TOOL" mutate "$RPC_DB0" "$player" "$key" GRANT 1001 3
mut_rc=$?
set -e
# 进程可能已 _exit；更新 inventory 若已死
if ! kill -0 "$FP_PID" 2>/dev/null; then
  FP_PID=""
fi

echo "== query via gamedb-1 (must see SUCCEEDED) =="
qout=""
ok_q=0
for _ in $(seq 1 30); do
  set +e
  qout="$("$TOOL" query "$RPC_DB1" "$player" "$key" GRANT 2>&1)"
  qrc=$?
  set -e
  echo "$qout"
  if [[ "$qrc" -eq 0 ]] && echo "$qout" | grep -q 'status=SUCCEEDED' && \
     echo "$qout" | grep -q 'completed_ok=1'; then
    ok_q=1
    break
  fi
  sleep 0.5
done
[[ "$ok_q" -eq 1 ]] || { echo "ERROR: query did not see SUCCEEDED after unknown"; exit 1; }
ver="$(echo "$qout" | sed -n 's/.*ver=\([0-9]*\).*/\1/p' | head -1)"
[[ -n "$ver" && "$ver" -gt 0 ]] || { echo "ERROR: bad version"; exit 1; }

echo "== ensure gamedb-0 dead; survivor write on gamedb-1 =="
if [[ -n "${FP_PID:-}" ]]; then
  kill -9 "$FP_PID" 2>/dev/null || true
  FP_PID=""
fi
sleep 1
if curl -fsS -m 2 "http://${HOST}:${HTTP_DB0}/health/live" >/dev/null 2>&1; then
  echo "ERROR: gamedb-0 still live"; exit 1
fi
curl -fsS -m 3 "http://${HOST}:${HTTP_DB1}/health/ready" | grep -qi ready

key2="${key}_next"
out2="$("$TOOL" mutate "$RPC_DB1" "$player" "$key2" GRANT 1001 1)"
echo "$out2"
echo "$out2" | grep -q 'ok=1' || { echo "ERROR: survivor mutate failed"; exit 1; }
ver2="$(echo "$out2" | sed -n 's/.*ver=\([0-9]*\).*/\1/p' | head -1)"
[[ -n "$ver2" && "$ver2" -gt "$ver" ]] || {
  echo "ERROR: version did not advance ($ver -> $ver2)"; exit 1
}

trap - EXIT
echo "test_gamedb_unknown_result_failover.sh PASS (mut_rc=$mut_rc)"
