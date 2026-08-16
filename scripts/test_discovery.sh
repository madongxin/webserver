#!/usr/bin/env bash
# 阶段 2：静态/advertise 发现冒烟（etcd v3 未落地前用 StaticServiceRegistry 配置）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== discovery policy =="
echo "prefer: etcd v3 lease/watch OR brpc NamingService (see PHASE2_STATUS.md)"
echo "current: StaticServiceRegistry + optional etcd v2 sync; do not extend v2"

GW_CNF="$ROOT/config/gateway.cnf"
if [[ -f "$GW_CNF" ]]; then
  if grep -qE 'logic_addrs=.+' "$GW_CNF"; then
    echo "ok: gateway.cnf has logic_addrs"
  else
    echo "WARN: gateway.cnf missing logic_addrs (ok if not running formal)"
  fi
fi

LOGIC_CNF="$ROOT/config/gamelogic.cnf"
SESSION_CNF="$ROOT/config/session.cnf"
check_push_addrs() {
  local cnf="$1" label="$2"
  if [[ -f "$cnf" ]] && grep -q 'gateway_push_addrs=' "$cnf"; then
    if grep -q '0.0.0.0' <<<"$(grep gateway_push_addrs "$cnf")"; then
      echo "FAIL: $label gateway_push_addrs must not use 0.0.0.0"
      exit 1
    fi
    echo "ok: $label gateway_push_addrs present without 0.0.0.0"
    return 0
  fi
  return 1
}
if check_push_addrs "$LOGIC_CNF" "gamelogic.cnf"; then
  :
elif check_push_addrs "$SESSION_CNF" "session.cnf"; then
  :
else
  echo "INFO: run start_formal.sh to materialize gateway_push_addrs"
fi

echo "test_discovery.sh PASS"
