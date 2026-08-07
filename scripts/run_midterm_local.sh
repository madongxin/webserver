#!/usr/bin/env bash
# 中期：session + gamedb + world + gamelogic + gateway 本地冒烟（Login）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

pick_bin() {
  local name="$1"
  if [[ -x "$ROOT/build/test/${name}" ]]; then
    echo "$ROOT/build/test/${name}"
  elif [[ -x "$ROOT/build/test/server" ]]; then
    echo "$ROOT/build/test/server"
  else
    echo ""
  fi
}

SESSION_BIN="$(pick_bin session)"
GAMEDB_BIN="$(pick_bin gamedb)"
WORLD_BIN="$(pick_bin world)"
LOGIC_BIN="$(pick_bin gamelogic)"
GW_BIN="$(pick_bin gateway)"
CLIENT="$ROOT/build/test/game_tcp_smoke_client"

if [[ -z "$GW_BIN" ]]; then
  echo "build first: ./scripts/build.sh Debug"
  exit 1
fi

HTTP_G=29080
GAME=29081
LOGIC=29201
HTTP_L=29090
HTTP_W=29091
HTTP_S=29092
HTTP_D=29093
WORLD=29301
SESSION=29401
GAMEDB=29501

WORKDIR=/tmp/gamemesh-midterm
mkdir -p "$WORKDIR"
GW_CNF="$ROOT/config/gateway.cnf"
WORLD_CNF="$ROOT/config/world.cnf"
SESSION_CNF="$ROOT/config/session.cnf"
GAMEDB_CNF="$ROOT/config/gamedb.cnf"
cp -a "$GW_CNF" "$WORKDIR/gateway.cnf.bak"
cp -a "$WORLD_CNF" "$WORKDIR/world.cnf.bak"
cp -a "$SESSION_CNF" "$WORKDIR/session.cnf.bak"
cp -a "$GAMEDB_CNF" "$WORKDIR/gamedb.cnf.bak"

printf '%s\n' \
  "logic_addrs=127.0.0.1:${LOGIC}" \
  "logic_instance_ids=gl-0" \
  "world_addrs=127.0.0.1:${WORLD}" \
  "session_addrs=127.0.0.1:${SESSION}" \
  "gamedb_addrs=127.0.0.1:${GAMEDB}" \
  "etcd_endpoints=" \
  "rpc_timeout_ms=3000" \
  "ssl_enable=0" >"$GW_CNF"
printf 'listen_addr=0.0.0.0:%s\nidle_timeout_sec=30\ngamedb_addrs=127.0.0.1:%s\nssl_enable=0\n' \
  "$WORLD" "$GAMEDB" >"$WORLD_CNF"
printf 'listen_addr=0.0.0.0:%s\nidle_timeout_sec=30\nssl_enable=0\n' "$SESSION" >"$SESSION_CNF"
printf 'listen_addr=0.0.0.0:%s\nidle_timeout_sec=30\nnats_url=\nssl_enable=0\n' "$GAMEDB" >"$GAMEDB_CNF"

PIDS=()
cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
  [[ -f "$WORKDIR/gateway.cnf.bak" ]] && mv -f "$WORKDIR/gateway.cnf.bak" "$GW_CNF"
  [[ -f "$WORKDIR/world.cnf.bak" ]] && mv -f "$WORKDIR/world.cnf.bak" "$WORLD_CNF"
  [[ -f "$WORKDIR/session.cnf.bak" ]] && mv -f "$WORKDIR/session.cnf.bak" "$SESSION_CNF"
  [[ -f "$WORKDIR/gamedb.cnf.bak" ]] && mv -f "$WORKDIR/gamedb.cnf.bak" "$GAMEDB_CNF"
}
trap cleanup EXIT

run_role() {
  local bin="$1" role="$2" logfile="$3"
  shift 3
  if [[ "$(basename "$bin")" == "server" ]]; then
    "$bin" "$role" "$@" >"$logfile" 2>&1 &
  else
    "$bin" "$@" >"$logfile" 2>&1 &
  fi
  PIDS+=($!)
}

run_role "$SESSION_BIN" session "$WORKDIR/session.log" "$HTTP_S"
run_role "$GAMEDB_BIN" gamedb "$WORKDIR/gamedb.log" "$HTTP_D"
run_role "$LOGIC_BIN" gamelogic "$WORKDIR/logic.log" "$HTTP_L" "$LOGIC"
run_role "$WORLD_BIN" world "$WORKDIR/world.log" "$HTTP_W"
sleep 2
run_role "$GW_BIN" gateway "$WORKDIR/gw.log" "$HTTP_G" "$GAME"
sleep 2

grep -E 'SessionBrpcServer listening|GameDbBrpcServer listening|GameLogicBrpcServer listening|WorldBrpcServer listening|SessionRpcClient ready|BrpcGameDbRepository ready|GameTcpGateway ready|FATAL|ERROR' \
  "$WORKDIR"/*.log || true

fail=0
grep -q 'SessionBrpcServer listening' "$WORKDIR/session.log" || { echo "FAIL: session"; fail=1; }
grep -q 'GameDbBrpcServer listening' "$WORKDIR/gamedb.log" || { echo "FAIL: gamedb"; fail=1; }
grep -q 'WorldBrpcServer listening' "$WORKDIR/world.log" || { echo "FAIL: world"; fail=1; }
grep -q 'GameLogicBrpcServer listening' "$WORKDIR/logic.log" || { echo "FAIL: gamelogic"; fail=1; }
[[ "$fail" -eq 0 ]] || exit 1

if [[ -x "$CLIENT" ]]; then
  "$CLIENT" 127.0.0.1 "$GAME" 10001
  echo "smoke client exit=$?"
fi

curl -fsS -m 3 "http://127.0.0.1:${HTTP_G}/metrics" >/dev/null && echo gateway_metrics_ok
echo "midterm local smoke done"
