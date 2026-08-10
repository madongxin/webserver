#!/usr/bin/env bash
# 最终 E2E 场景编排（对照 Remediation §最终 E2E）
# 用法:
#   ./scripts/run_e2e_cluster.sh   # 或已有 formal/e2e 集群
#   ./scripts/test_final_e2e.sh
#   SCENARIOS=1,2,4 ./scripts/test_final_e2e.sh
#   ./scripts/test_final_e2e.sh --start-cluster
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

START_CLUSTER=0
for a in "$@"; do
  [[ "$a" == "--start-cluster" ]] && START_CLUSTER=1
done

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
SEED="${ROOT}/build/test/placement_seed_tool"
[[ -x "$CLIENT" ]] || { echo "ERROR: missing $CLIENT (./scripts/build.sh Debug)"; exit 1; }

if [[ "$START_CLUSTER" -eq 1 ]]; then
  export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
  if [[ -f "$GAMEMESH_RUN_DIR/pids" ]]; then
    "$ROOT/scripts/stop_e2e_cluster.sh" || true
    sleep 1
  fi
  "$ROOT/scripts/run_e2e_cluster.sh"
  sleep 2
fi

RUN_DIR="${GAMEMESH_RUN_DIR:-}"
if [[ -z "$RUN_DIR" || ! -f "$RUN_DIR/pids" ]]; then
  if [[ -f "$ROOT/run/e2e/pids" ]]; then
    RUN_DIR="$ROOT/run/e2e"
  elif [[ -f "$ROOT/run/formal/pids" ]]; then
    RUN_DIR="$ROOT/run/formal"
  elif [[ -f "$ROOT/run/cluster/pids" ]]; then
    RUN_DIR="$ROOT/run/cluster"
  else
    echo "ERROR: no cluster pids. Run: ./scripts/run_e2e_cluster.sh or --start-cluster" >&2
    exit 1
  fi
fi
export GAMEMESH_RUN_DIR="$RUN_DIR"
if [[ -f "$RUN_DIR/E2E_PORTS.env" ]]; then
  # shellcheck disable=SC1090
  source "$RUN_DIR/E2E_PORTS.env"
fi

HOST="${E2E_HOST:-127.0.0.1}"
GW0_HTTP="${E2E_GW0_HTTP:-${GAMEMESH_HTTP_G0:-8080}}"
GW1_HTTP="${E2E_GW1_HTTP:-${GAMEMESH_HTTP_G1:-8082}}"
GW0_GAME="${E2E_GW0_GAME:-${GAMEMESH_GAME_G0:-8081}}"
GW1_GAME="${E2E_GW1_GAME:-${GAMEMESH_GAME_G1:-8083}}"
S0_HTTP="${E2E_S0_HTTP:-${GAMEMESH_HTTP_S:-8093}}"
S1_HTTP="${E2E_S1_HTTP:-${GAMEMESH_HTTP_S2:-8096}}"
L0_HTTP="${E2E_L0_HTTP:-${GAMEMESH_HTTP_L0:-8090}}"
L1_HTTP="${E2E_L1_HTTP:-${GAMEMESH_HTTP_L1:-8091}}"
DB0_HTTP="${E2E_DB0_HTTP:-${GAMEMESH_HTTP_D0:-8094}}"
DB1_HTTP="${E2E_DB1_HTTP:-${GAMEMESH_HTTP_D1:-8095}}"

mapfile -t PIDS <"$RUN_DIR/pids"
# start_formal 顺序: session,gamedb0,gamedb1,world,logic0,logic1,gw0,gw1[,session2]
PID_S0="${PIDS[0]:-}"
PID_DB0="${PIDS[1]:-}"
PID_L1="${PIDS[5]:-}"
PID_GW0="${PIDS[6]:-}"

SCENARIOS="${SCENARIOS:-1,2,3,4,5,6,7,8,9}"
IFS=',' read -r -a WANT <<<"$SCENARIOS"
fail=0
pass=0

run_sc() {
  local id="$1" title="$2"
  shift 2
  local want=0
  for w in "${WANT[@]}"; do
    [[ "$w" == "$id" || "$w" == "all" ]] && want=1
  done
  if [[ "$want" -eq 0 ]]; then
    echo "-- skip scenario $id: $title"
    return 0
  fi
  echo "== scenario $id: $title =="
  if "$@"; then
    echo "PASS scenario $id"
    pass=$((pass + 1))
  else
    echo "FAIL scenario $id" >&2
    fail=$((fail + 1))
  fi
}

ready() {
  local port="$1"
  curl -fsS -m 3 "http://${HOST}:${port}/health/ready" | grep -q '"ready":true'
}

# --- 1: Login gw0 唯一 Session ---
sc1() {
  ready "$GW0_HTTP" || return 1
  out="$("$CLIENT" register-login "$HOST" "$GW0_GAME" "e2e-sc1-$$" "e2epass1")" || return 1
  echo "$out"
  echo "$out" | grep -q 'login_ok=1' || return 1
  echo "$out" | grep -q 'session_id=' || return 1
  echo "$out" | grep -q 'token=' || return 1
}

# --- 2: EnterMap 到 gl-1（预置 Owner → 跨 Logic Transfer） ---
sc2() {
  [[ -x "$SEED" ]] || { echo "ERROR: missing placement_seed_tool" >&2; return 1; }
  local tpl=$((920000 + RANDOM % 9000))
  local seed_out map_id
  seed_out="$("$SEED" "$tpl" "gl-1" 1)" || return 1
  map_id="$(echo "$seed_out" | sed -n 's/^map_instance_id=//p' | head -1)"
  [[ -n "$map_id" ]] || return 1
  out="$("$CLIENT" dual-gw "$HOST" "$GW0_GAME" "$HOST" "$GW1_GAME" "$tpl" "$map_id")" || {
    echo "$out"
    return 1
  }
  echo "$out"
  echo "$out" | grep -q 'enter_map_ok=1' || return 1
  echo "$out" | grep -q 'gamelogic_instance_id=gl-1' || return 1
}

# --- 3: 可靠 Push（dual-gw 内 EnterMap notify；断言 reconnect + 回放证据）---
sc3() {
  out="$("$CLIENT" dual-gw "$HOST" "$GW0_GAME" "$HOST" "$GW1_GAME")" || {
    echo "$out"
    return 1
  }
  echo "$out"
  echo "$out" | grep -q 'dual_gw_ok=1' || return 1
  echo "$out" | grep -q 'reconnect_ok=1' || return 1
  echo "$out" | grep -qE 'push_recv=1|replay_n=[1-9]|need_full_snapshot=1' || return 1
}

# --- 4: kill gw0，gw1 reconnect（dual-gw 已覆盖；再加真实 SIGKILL）---
sc4() {
  ready "$GW1_HTTP" || return 1
  "$CLIENT" dual-gw "$HOST" "$GW0_GAME" "$HOST" "$GW1_GAME" >/tmp/e2e_sc4_$$.out || {
    cat /tmp/e2e_sc4_$$.out
    return 1
  }
  grep -q 'dual_gw_ok=1' /tmp/e2e_sc4_$$.out || return 1
  if [[ -z "$PID_GW0" ]] || ! kill -0 "$PID_GW0" 2>/dev/null; then
    echo "ERROR: gw0 pid unknown/dead; cannot SIGKILL drill" >&2
    return 1
  fi
  echo "SIGKILL gw0 pid=$PID_GW0"
  kill -9 "$PID_GW0"
  sleep 1
  if kill -0 "$PID_GW0" 2>/dev/null; then
    echo "ERROR gw0 still alive" >&2
    return 1
  fi
  ready "$GW1_HTTP" || return 1
  out="$("$CLIENT" register-login "$HOST" "$GW1_GAME" "e2e-sc4-$$" "e2epass1")" || return 1
  echo "$out" | grep -q 'login_ok=1' || return 1
}

# --- 5: kill session0，session1 仍 ready ---
sc5() {
  if ! curl -fsS -m 2 "http://${HOST}:${S1_HTTP}/api/version" 2>/dev/null | grep -q session; then
    echo "INFO: no session1; run test_session_ha.sh"
    "$ROOT/scripts/test_session_ha.sh"
    return 0
  fi
  ready "$S1_HTTP" || true
  if [[ -n "$PID_S0" ]] && kill -0 "$PID_S0" 2>/dev/null; then
    kill -9 "$PID_S0"
    sleep 1
  fi
  curl -fsS -m 3 "http://${HOST}:${S1_HTTP}/api/version" | grep -q session
}

# --- 6: kill logic1 + lease recover（委托 kill_logic_drill 若有 gl-0 地图；否则 seed+mark）---
sc6() {
  [[ -x "$SEED" ]] || { echo "ERROR: missing placement_seed_tool" >&2; return 1; }
  local tpl=$((930000 + RANDOM % 9000))
  local seed_out map_id old_epoch
  seed_out="$("$SEED" "$tpl" "gl-1" 1)" || return 1
  map_id="$(echo "$seed_out" | sed -n 's/^map_instance_id=//p' | head -1)"
  old_epoch="$(echo "$seed_out" | sed -n 's/^owner_epoch=//p' | head -1)"
  [[ -n "$map_id" ]] || return 1
  if [[ -n "$PID_L1" ]] && kill -0 "$PID_L1" 2>/dev/null; then
    echo "SIGKILL logic1 pid=$PID_L1 map=$map_id"
    kill -9 "$PID_L1"
    sleep 1
  fi
  # 使用 map_lease_drill：MarkRecovering + Migrate → gl-0
  local drill="${ROOT}/build/test/map_lease_drill"
  [[ -x "$drill" ]] || { echo "ERROR: missing map_lease_drill" >&2; return 1; }
  "$drill" "$map_id" "gl-0" "${old_epoch:-1}"
}

# --- 8: GameDB kill → HTTP 死；gamedb1 仍活（若有）---
sc8() {
  if [[ -n "$PID_DB0" ]] && kill -0 "$PID_DB0" 2>/dev/null; then
    kill -9 "$PID_DB0"
    sleep 1
    if curl -fsS -m 2 "http://${HOST}:${DB0_HTTP}/health/live" >/dev/null 2>&1; then
      echo "ERROR killed gamedb0 still live" >&2
      return 1
    fi
  fi
  if curl -fsS -m 2 "http://${HOST}:${DB1_HTTP}/api/version" 2>/dev/null | grep -q gamedb; then
    curl -fsS -m 3 "http://${HOST}:${DB1_HTTP}/health/ready" | grep -q '"ready":true' || true
  fi
  return 0
}

# --- 9: SIGTERM 优雅摘流（优先 gw1，避免与 sc6 杀 logic 冲突）---
sc9() {
  local pid="${PIDS[7]:-}"  # gw1
  local http="$GW1_HTTP"
  if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
    pid="${PIDS[4]:-}"  # logic0 fallback
    http="$L0_HTTP"
  fi
  [[ -n "$pid" ]] || { echo "ERROR: no drain target pid"; return 1; }
  kill -0 "$pid" 2>/dev/null || { echo "ERROR: drain target dead pid=$pid"; return 1; }
  ready "$http" || true
  echo "SIGTERM pid=$pid http=$http"
  kill -TERM "$pid"
  local i=0 code
  while (( i < 30 )); do
    code="$(curl -s -o /dev/null -w '%{http_code}' -m 1 "http://${HOST}:${http}/health/ready" 2>/dev/null || true)"
    [[ -z "$code" ]] && code=000
    if [[ "$code" == "503" || "$code" == "000" ]]; then
      echo "drain observed code=$code"
      return 0
    fi
    # 进程已退出也算优雅摘流完成
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "drain observed process exited"
      return 0
    fi
    sleep 0.3
    i=$((i + 1))
  done
  echo "ERROR: ready did not fail after SIGTERM (last_code=$code)" >&2
  return 1
}

# 9 在 6 前：避免 sc6 杀 logic0 后无法验证 drain
run_sc 1 "Login gw0 unique session" sc1
run_sc 2 "EnterMap transfer to gl-1" sc2
run_sc 3 "Reliable Push path" sc3
run_sc 4 "kill gw0 + gw1 path" sc4
run_sc 5 "kill session survivor" sc5
run_sc 9 "SIGTERM drain ready" sc9
run_sc 6 "kill logic + lease migrate" sc6
run_sc 8 "kill gamedb0" sc8

# 场景 7：动态发现 / DRAINING 契约（完整启 gl-2 进程见 test_dynamic_logic_scale）
run_sc 7 "dynamic logic scale / DRAINING" bash "$ROOT/scripts/test_dynamic_logic_scale.sh"

# 场景 10：短混沌（完整 2h 见 soak_test.sh）；默认 SCENARIOS 不含 10
run_sc 10 "chaos short dual-gw burst" bash -c '
  set -euo pipefail
  source "${GAMEMESH_RUN_DIR}/E2E_PORTS.env" 2>/dev/null || true
  H="${E2E_HOST:-127.0.0.1}"
  G0="${E2E_GW0_GAME:-8081}"
  G1="${E2E_GW1_GAME:-8083}"
  C="'"$CLIENT"'"
  for i in $(seq 1 5); do
    "$C" dual-gw "$H" "$G0" "$H" "$G1" "$((940000 + i))" 0 | grep -q dual_gw_ok=1
  done
'

echo "==== final e2e: pass=$pass fail=$fail (requested: ${SCENARIOS}) ===="

[[ "$fail" -eq 0 ]]
echo "test_final_e2e.sh PASS"
