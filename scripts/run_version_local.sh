#!/usr/bin/env bash
# 本版目标冒烟：2×gateway + 2×gamelogic + 2×gamedb + session + world
# 覆盖：多开、注册、登录
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

pick_bin() {
  local name="$1"
  if [[ -x "$ROOT/build/test/${name}" ]]; then echo "$ROOT/build/test/${name}"
  elif [[ -x "$ROOT/build/test/server" ]]; then echo "$ROOT/build/test/server"
  else echo ""; fi
}

SESSION_BIN="$(pick_bin session)"
GAMEDB_BIN="$(pick_bin gamedb)"
WORLD_BIN="$(pick_bin world)"
LOGIC_BIN="$(pick_bin gamelogic)"
GW_BIN="$(pick_bin gateway)"
CLIENT="$ROOT/build/test/game_tcp_smoke_client"

[[ -n "$GW_BIN" ]] || { echo "build first: ./scripts/build.sh Debug"; exit 1; }

HTTP_G0=29080; GAME0=29081
HTTP_G1=29082; GAME1=29083
HTTP_L0=29090; LOGIC0=29201
HTTP_L1=29091; LOGIC1=29202
HTTP_W=29092; WORLD=29301
HTTP_S=29093; SESSION=29401
HTTP_D0=29094; GAMEDB0=29501
HTTP_D1=29095; GAMEDB1=29502

WORKDIR=/tmp/gamemesh-version
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
  "logic_addrs=127.0.0.1:${LOGIC0},127.0.0.1:${LOGIC1}" \
  "logic_instance_ids=gl-0,gl-1" \
  "world_addrs=127.0.0.1:${WORLD}" \
  "session_addrs=127.0.0.1:${SESSION}" \
  "gamedb_addrs=127.0.0.1:${GAMEDB0},127.0.0.1:${GAMEDB1}" \
  "etcd_endpoints=" \
  "rpc_timeout_ms=3000" \
  "ssl_enable=0" >"$GW_CNF"
printf 'listen_addr=0.0.0.0:%s\nidle_timeout_sec=30\ngamedb_addrs=127.0.0.1:%s,127.0.0.1:%s\nssl_enable=0\n' \
  "$WORLD" "$GAMEDB0" "$GAMEDB1" >"$WORLD_CNF"
printf 'listen_addr=0.0.0.0:%s\nidle_timeout_sec=30\nssl_enable=0\n' "$SESSION" >"$SESSION_CNF"

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
  local bin="$1" role="$2" logfile="$3"; shift 3
  if [[ "$(basename "$bin")" == "server" ]]; then
    "$bin" "$role" "$@" >"$logfile" 2>&1 &
  else
    "$bin" "$@" >"$logfile" 2>&1 &
  fi
  PIDS+=($!)
}

run_role "$SESSION_BIN" session "$WORKDIR/session.log" "$HTTP_S" "$SESSION"
run_role "$GAMEDB_BIN" gamedb "$WORKDIR/gamedb0.log" "$HTTP_D0" "$GAMEDB0"
run_role "$GAMEDB_BIN" gamedb "$WORKDIR/gamedb1.log" "$HTTP_D1" "$GAMEDB1"
run_role "$LOGIC_BIN" gamelogic "$WORKDIR/logic0.log" "$HTTP_L0" "$LOGIC0"
run_role "$LOGIC_BIN" gamelogic "$WORKDIR/logic1.log" "$HTTP_L1" "$LOGIC1"
run_role "$WORLD_BIN" world "$WORKDIR/world.log" "$HTTP_W"
sleep 2
run_role "$GW_BIN" gateway "$WORKDIR/gw0.log" "$HTTP_G0" "$GAME0"
run_role "$GW_BIN" gateway "$WORKDIR/gw1.log" "$HTTP_G1" "$GAME1"
sleep 2

grep -E 'listening|SessionRpcClient ready|BrpcGameDbRepository ready|channels=|GameTcpGateway ready|FATAL|ERROR' \
  "$WORKDIR"/*.log || true

fail=0
grep -q 'SessionBrpcServer listening' "$WORKDIR/session.log" || { echo "FAIL: session"; fail=1; }
grep -q 'GameDbBrpcServer listening' "$WORKDIR/gamedb0.log" || { echo "FAIL: gamedb0"; fail=1; }
grep -q 'GameDbBrpcServer listening' "$WORKDIR/gamedb1.log" || { echo "FAIL: gamedb1"; fail=1; }
grep -q 'GameLogicBrpcServer listening' "$WORKDIR/logic0.log" || { echo "FAIL: logic0"; fail=1; }
grep -q 'GameLogicBrpcServer listening' "$WORKDIR/logic1.log" || { echo "FAIL: logic1"; fail=1; }
grep -q 'WorldBrpcServer listening' "$WORKDIR/world.log" || { echo "FAIL: world"; fail=1; }
grep -q 'GameTcpGateway ready' "$WORKDIR/gw0.log" || { echo "FAIL: gateway0"; fail=1; }
grep -q 'GameTcpGateway ready' "$WORKDIR/gw1.log" || { echo "FAIL: gateway1"; fail=1; }
[[ "$fail" -eq 0 ]] || exit 1

if [[ -x "$CLIENT" ]]; then
  "$CLIENT" 127.0.0.1 "$GAME0" register "ver-gw0-$$"
  echo "gateway0 register+login exit=$?"
  "$CLIENT" 127.0.0.1 "$GAME1" register "ver-gw1-$$"
  echo "gateway1 register+login exit=$?"
fi

curl -fsS -m 3 "http://127.0.0.1:${HTTP_G0}/metrics" >/dev/null && echo gateway0_metrics_ok
curl -fsS -m 3 "http://127.0.0.1:${HTTP_G1}/metrics" >/dev/null && echo gateway1_metrics_ok
echo "version local smoke done (2x gateway + 2x logic + 2x gamedb + world + session)"
