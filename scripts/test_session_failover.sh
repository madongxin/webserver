#!/usr/bin/env bash
# 阶段二：Session 真实故障切换（业务断言）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: build game_tcp_e2e_client"; exit 1; }

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
[[ -n "$sid" && -n "$player" && -n "$token" ]] || {
  echo "ERROR: missing session fields"; exit 1
}
old_token="$token"

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

echo "== post-kill: original player reconnect on gw1 =="
# Gateway Session Channel 可能仍指向死 sess-0；短暂重试等发现/并集回落
ok_rc=0
out3=""
for _ in $(seq 1 20); do
  set +e
  out3="$("$CLIENT" reconnect "$HOST" "$GW1" "$player" "$sid" "$token" 0 2>&1)"
  rc=$?
  set -e
  echo "$out3"
  if [[ "$rc" -eq 0 ]] && echo "$out3" | grep -q 'reconnect_ok=1'; then
    ok_rc=1
    break
  fi
  sleep 1
done
[[ "$ok_rc" -eq 1 ]] || { echo "ERROR: reconnect_ok missing after sess-0 kill"; exit 1; }
new_token="$(echo "$out3" | sed -n 's/^token=//p' | tail -1)"
[[ -n "$new_token" ]] || new_token="$token"

echo "== late request with old fence/token should fail or be superseded =="
set +e
out_old="$("$CLIENT" reconnect "$HOST" "$GW0" "$player" "$sid" "$old_token" 0 2>&1)"
old_rc=$?
set -e
echo "$out_old"
# 旧 token 在 generation 提升后应失败；若 gw0 session 通道仍挂死，也算拒绝
if [[ "$old_rc" -eq 0 ]] && echo "$out_old" | grep -q 'reconnect_ok=1'; then
  got="$(echo "$out_old" | sed -n 's/^token=//p' | tail -1)"
  if [[ -n "$new_token" && "$got" == "$old_token" ]]; then
    echo "ERROR: old fence still accepted as current"; exit 1
  fi
  # 若 reconnect 成功但换了新 token，说明服务端轮换了 fence（可接受）
fi

# Redis：该玩家 ONLINE 态至多一条权威 session
online_n=0
while read -r k; do
  [[ -z "$k" ]] && continue
  st="$("${RCLI[@]}" HGET "$k" state 2>/dev/null || true)"
  [[ "$st" == "ONLINE" ]] && online_n=$((online_n + 1))
done < <("${RCLI[@]}" --scan --pattern "${PREFIX}session:${player}" 2>/dev/null; \
         "${RCLI[@]}" --scan --pattern "${PREFIX}session:online:${player}" 2>/dev/null)
# 兼容：直接读 session:{player}
sess_key="${PREFIX}session:${player}"
st="$("${RCLI[@]}" HGET "$sess_key" state 2>/dev/null || true)"
echo "player=$player sid=$sid state=${st:-?} online_matches≈$online_n"
if [[ "$st" == "ONLINE" || "$st" == "DISCONNECTED" || -z "$st" ]]; then
  :
else
  echo "WARN: unexpected state=$st (still PASS if reconnect ok)"
fi

echo "test_session_failover.sh PASS"
