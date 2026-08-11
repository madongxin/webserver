#!/usr/bin/env bash
# 阶段二：Session 真实故障切换（业务断言，禁止 WARN-but-PASS）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
DISPATCH="${ROOT}/build/test/logic_dispatch_tool"
[[ -x "$CLIENT" ]] || { echo "ERROR: build game_tcp_e2e_client"; exit 1; }
[[ -x "$DISPATCH" ]] || { echo "ERROR: build logic_dispatch_tool"; exit 1; }

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME}"
GW1="${E2E_GW1_GAME}"
S0_HTTP="$(e2e_http_of session sess-0 || echo "${E2E_S0_HTTP}")"
S1_HTTP="$(e2e_http_of session sess-1 || echo "${E2E_S1_HTTP}")"
PID_S0="$(e2e_pid_of session sess-0)" || { echo "ERROR: sess-0 missing in inventory"; exit 1; }

REDIS_PASS=""
[[ -f "$ROOT/config/redis.cnf" ]] && REDIS_PASS="$(awk -F= '/^password=/{print $2; exit}' "$ROOT/config/redis.cnf")"
RCLI=(redis-cli)
[[ -n "$REDIS_PASS" ]] && RCLI=(redis-cli -a "$REDIS_PASS" --no-auth-warning)
PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"

echo "== pre: login on gw0 =="
dev="sessfail_$$"
out1="$("$CLIENT" register-login "$HOST" "$GW0" "$dev" "pw_sess_fail1")"
echo "$out1"
echo "$out1" | grep -q 'login_ok=1' || { echo "ERROR: pre login failed"; exit 1; }
sid="$(echo "$out1" | sed -n 's/^session_id=//p' | head -1)"
player="$(echo "$out1" | sed -n 's/^player_id=//p' | head -1)"
token="$(echo "$out1" | sed -n 's/^token=//p' | head -1)"
old_gen="$(echo "$out1" | sed -n 's/^generation=//p' | head -1)"
[[ -n "$sid" && -n "$player" && -n "$token" && -n "$old_gen" ]] || {
  echo "ERROR: missing session fields"; exit 1
}
old_token="$token"
old_sid="$sid"

echo "== kill session sess-0 pid=$PID_S0 =="
kill -9 "$PID_S0"
sleep 1
if kill -0 "$PID_S0" 2>/dev/null; then
  echo "ERROR: sess-0 still alive"; exit 1
fi
if curl -fsS -m 2 "http://${HOST}:${S0_HTTP}/api/version" >/dev/null 2>&1; then
  echo "ERROR: sess-0 still answering"; exit 1
fi
curl -fsS -m 3 "http://${HOST}:${S1_HTTP}/api/version" | grep -qi session

echo "== post-kill: new login via gw1 (sess-1) =="
out2="$("$CLIENT" register-login "$HOST" "$GW1" "sessfail_new_$$" "pw_new_ok")"
echo "$out2"
echo "$out2" | grep -q 'login_ok=1' || { echo "ERROR: post-kill login failed"; exit 1; }

echo "== post-kill: original player reconnect on gw1 (hold connection for ONLINE assert) =="
rc_tmp="$(mktemp)"
: >"$rc_tmp"
E2E_HOLD_MS=8000 "$CLIENT" reconnect "$HOST" "$GW1" "$player" "$sid" "$token" 0 >"$rc_tmp" 2>&1 &
rc_pid=$!
saw=0
for _ in $(seq 1 80); do
  if grep -q 'reconnect_ok=1' "$rc_tmp" 2>/dev/null; then
    saw=1
    break
  fi
  if grep -q '^error=' "$rc_tmp" 2>/dev/null && ! kill -0 "$rc_pid" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$rc_pid" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
out3="$(cat "$rc_tmp")"
echo "$out3"
[[ "$saw" -eq 1 ]] || {
  wait "$rc_pid" 2>/dev/null || true
  rm -f "$rc_tmp"
  echo "ERROR: reconnect_ok missing after sess-0 kill"; exit 1
}
new_token="$(echo "$out3" | sed -n 's/^token=//p' | tail -1)"
new_sid="$(echo "$out3" | sed -n 's/^session_id=//p' | tail -1)"
new_gen="$(echo "$out3" | sed -n 's/^generation=//p' | tail -1)"
[[ -n "$new_token" ]] || new_token="$token"
[[ -n "$new_sid" ]] || new_sid="$sid"
[[ -n "$new_gen" ]] || {
  wait "$rc_pid" 2>/dev/null || true
  rm -f "$rc_tmp"
  echo "ERROR: missing generation"; exit 1
}

sess_key="${PREFIX}session:${player}"
st=""
redis_sid=""
redis_tok=""
redis_gen=""
logic_id=""
for _ in $(seq 1 50); do
  st="$("${RCLI[@]}" HGET "$sess_key" state 2>/dev/null || true)"
  redis_sid="$("${RCLI[@]}" HGET "$sess_key" sessionId 2>/dev/null || true)"
  redis_tok="$("${RCLI[@]}" HGET "$sess_key" token 2>/dev/null || true)"
  redis_gen="$("${RCLI[@]}" HGET "$sess_key" generation 2>/dev/null || true)"
  logic_id="$("${RCLI[@]}" HGET "$sess_key" gamelogicInstanceId 2>/dev/null || true)"
  if [[ "$st" == "ONLINE" && "$redis_tok" == "$new_token" && "$redis_gen" == "$new_gen" ]]; then
    break
  fi
  sleep 0.1
done
echo "player=$player state=$st sid=$redis_sid gen=$redis_gen logic=$logic_id (held)"
wait "$rc_pid" || true
rm -f "$rc_tmp"
[[ "$st" == "ONLINE" ]] || { echo "ERROR: expected ONLINE got '${st:-}'"; exit 1; }
[[ "$redis_sid" == "$new_sid" ]] || { echo "ERROR: sessionId mismatch"; exit 1; }
[[ "$redis_tok" == "$new_token" ]] || { echo "ERROR: token mismatch"; exit 1; }
[[ "$redis_gen" == "$new_gen" ]] || { echo "ERROR: generation mismatch"; exit 1; }

# 权威 session 键存在（扫描同 player 键）
key_n=0
while read -r k; do
  [[ -z "$k" ]] && continue
  [[ "$k" == "$sess_key" ]] && key_n=$((key_n + 1))
done < <("${RCLI[@]}" --scan --pattern "${PREFIX}session:${player}" 2>/dev/null)
[[ "$key_n" -eq 1 ]] || { echo "ERROR: expected exactly 1 session key, got $key_n"; exit 1; }

echo "== old fence Dispatch must FENCE_REJECT =="
[[ -n "$logic_id" ]] || { echo "ERROR: missing gamelogicInstanceId"; exit 1; }
logic_rpc="$(e2e_rpc_of gamelogic "$logic_id" 2>/dev/null || true)"
if [[ -z "$logic_rpc" ]]; then
  # 回落：尝试 inventory 中任意存活 logic
  logic_rpc="$(e2e_rpc_of gamelogic gl-0 2>/dev/null || e2e_rpc_of gamelogic gl-1 2>/dev/null || true)"
fi
[[ -n "$logic_rpc" ]] || { echo "ERROR: cannot resolve logic rpc for $logic_id"; exit 1; }
set +e
dout="$("$DISPATCH" "$logic_rpc" "$player" "$old_sid" "$old_token" "$old_gen" 2>&1)"
drc=$?
set -e
echo "$dout"
[[ "$drc" -eq 0 ]] || { echo "ERROR: dispatch tool rpc failed"; exit 1; }
echo "$dout" | grep -q 'error_code=FENCE_REJECT' || {
  echo "ERROR: expected FENCE_REJECT for old fence Dispatch"; exit 1
}

echo "test_session_failover.sh PASS"
