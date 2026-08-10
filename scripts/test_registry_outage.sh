#!/usr/bin/env bash
# 阶段二：Registry 发现键临时清空 — 保留 Channel；Dispatch 仍成功
# 限制：Session/Placement 与 Registry 共用同一 Redis；本测试只删 svc:gamelogic:*，
# 不停 Redis，避免把状态库故障伪装成「仅 Registry」故障。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: missing client"; exit 1; }
[[ -x "$ROOT/build/test/channel_snapshot_race_test" ]] || {
  echo "ERROR: missing channel_snapshot_race_test"; exit 1
}
"$ROOT/build/test/channel_snapshot_race_test"
[[ -x "$ROOT/build/test/phase1_channel_hold_test" ]] && \
  "$ROOT/build/test/phase1_channel_hold_test"

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"

HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME}"
GW1="${E2E_GW1_GAME}"
PREFIX="${GAMEMESH_REDIS_PREFIX:-gamemesh:dev:}"
REDIS_PASS=""
[[ -f "$ROOT/config/redis.cnf" ]] && REDIS_PASS="$(awk -F= '/^password=/{print $2; exit}' "$ROOT/config/redis.cnf")"
RCLI=(redis-cli)
[[ -n "$REDIS_PASS" ]] && RCLI=(redis-cli -a "$REDIS_PASS" --no-auth-warning)

bak="$(mktemp -d)"
restored=0
restore_registry() {
  [[ "$restored" -eq 1 ]] && return 0
  restored=1
  if [[ -f "$bak/index_members" ]]; then
    while read -r id; do
      [[ -z "$id" ]] && continue
      "${RCLI[@]}" SADD "${PREFIX}svcidx:gamelogic" "$id" >/dev/null 2>&1 || true
    done <"$bak/index_members"
  fi
  for meta in "$bak"/*.meta; do
    [[ -f "$meta" ]] || continue
    key="$(cat "$meta")"
    fields="${meta%.meta}.fields"
    [[ -f "$fields" ]] || continue
    # shellcheck disable=SC2046
    "${RCLI[@]}" HMSET "$key" $(cat "$fields") >/dev/null 2>&1 || true
    "${RCLI[@]}" EXPIRE "$key" 120 >/dev/null 2>&1 || true
  done
}
trap restore_registry EXIT

echo "== pre: login + dual-gw (channels warm) =="
out1="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$((960000 + RANDOM % 1000))" 0)"
echo "$out1"
echo "$out1" | grep -q 'login_ok=1' || { echo "ERROR: pre login"; exit 1; }
echo "$out1" | grep -q 'dual_gw_ok=1' || { echo "ERROR: pre dual-gw"; exit 1; }

echo "== backup + wipe discovery svc:gamelogic:* (keep session/placement) =="
"${RCLI[@]}" SMEMBERS "${PREFIX}svcidx:gamelogic" >"$bak/index_members" || true
n=0
while read -r k; do
  [[ -z "$k" ]] && continue
  n=$((n + 1))
  echo "$k" >"$bak/${n}.meta"
  # HGETALL → field value 行，折叠为 HMSET 参数
  "${RCLI[@]}" HGETALL "$k" | paste -d' ' - - >"$bak/${n}.fields" || true
  "${RCLI[@]}" DEL "$k" >/dev/null
done < <("${RCLI[@]}" --scan --pattern "${PREFIX}svc:gamelogic:*")
"${RCLI[@]}" DEL "${PREFIX}svcidx:gamelogic" >/dev/null || true
# 空发现不得清空 Channel：等待一轮 Gateway poll
sleep 6

echo "== dispatch still works with empty discovery =="
out2="$("$CLIENT" dual-gw "$HOST" "$GW0" "$HOST" "$GW1" "$((961000 + RANDOM % 1000))" 0)"
echo "$out2"
echo "$out2" | grep -q 'login_ok=1' || { echo "ERROR: login after registry wipe"; exit 1; }
echo "$out2" | grep -q 'dual_gw_ok=1' || { echo "ERROR: dual-gw after registry wipe"; exit 1; }

echo "== restore registry keys =="
restore_registry
trap - EXIT
sleep 2

# 恢复后写入带 version 的新发现条目，确认可被 Discover 读到
ver_id="gl-regtest-$$"
"${RCLI[@]}" HMSET "${PREFIX}svc:gamelogic:${ver_id}" \
  service gamelogic instance_id "$ver_id" address "127.0.0.1:19999" \
  port 19999 protocol baidu_std status UP version "v-restore-$$" >/dev/null
"${RCLI[@]}" SADD "${PREFIX}svcidx:gamelogic" "$ver_id" >/dev/null
"${RCLI[@]}" EXPIRE "${PREFIX}svc:gamelogic:${ver_id}" 60 >/dev/null
got_ver="$("${RCLI[@]}" HGET "${PREFIX}svc:gamelogic:${ver_id}" version)"
[[ "$got_ver" == "v-restore-$$" ]] || { echo "ERROR: restore discover version missing"; exit 1; }
"${RCLI[@]}" DEL "${PREFIX}svc:gamelogic:${ver_id}" >/dev/null
"${RCLI[@]}" SREM "${PREFIX}svcidx:gamelogic" "$ver_id" >/dev/null

echo "test_registry_outage.sh PASS (discovery keys only; shared Redis limitation documented)"
