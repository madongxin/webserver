#!/usr/bin/env bash
# Isolated TCP/protobuf login + enter stress.
# Uses build/test/game_tcp_e2e_client line-press (register, login, EnterMap).
# Does not call scripts/load_test.sh and does not touch the public EIP.
# Formal cluster (run/formal/pids) is always refused.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SCENARIO="login_enter"
SEED="${GAMEMESH_STRESS_SEED:-1}"
USERS=2
RAMP_SECONDS=0
TARGET_LOGIN_RPS=0
HOLD_SECONDS=0
MAP_TEMPLATE_ID=1002
SPAWN_DISTRIBUTION="stacked"
MOVE_HZ=0
GATEWAY_ENDPOINTS="127.0.0.1:8081"
CHAOS_ENABLED=0
EXECUTE=0
OUT="${ROOT}/run/stress/login_report.json"

usage() {
  cat <<'EOF'
usage: scripts/load_game_login_stress.sh [options]
  --scenario NAME
  --seed N
  --users N
  --ramp-seconds N
  --target-login-rps N
  --hold-seconds N
  --map-template-id N
  --spawn-distribution stacked|scatter
  --move-hz N
  --gateway-endpoints host:port[,host:port]
  --chaos-enabled 0|1
  --execute
  --out PATH
Without --execute the script writes a NOT_RUN report and exits 0.
--execute is refused when run/formal/pids exists, when an endpoint contains
124.222.244.169, or when the host is not loopback.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario) SCENARIO="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    --users) USERS="$2"; shift 2 ;;
    --ramp-seconds) RAMP_SECONDS="$2"; shift 2 ;;
    --target-login-rps) TARGET_LOGIN_RPS="$2"; shift 2 ;;
    --hold-seconds) HOLD_SECONDS="$2"; shift 2 ;;
    --map-template-id) MAP_TEMPLATE_ID="$2"; shift 2 ;;
    --spawn-distribution) SPAWN_DISTRIBUTION="$2"; shift 2 ;;
    --move-hz) MOVE_HZ="$2"; shift 2 ;;
    --gateway-endpoints) GATEWAY_ENDPOINTS="$2"; shift 2 ;;
    --chaos-enabled) CHAOS_ENABLED="$2"; shift 2 ;;
    --execute) EXECUTE=1; shift ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

mkdir -p "$(dirname "$OUT")"
CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
STATUS="NOT_RUN"
REASON="dry-run; pass --execute only against an isolated loopback cluster"
HOST="${GATEWAY_ENDPOINTS%%,*}"
HOST="${HOST%%:*}"
PORT="${GATEWAY_ENDPOINTS%%,*}"
PORT="${PORT##*:}"

write_report() {
  cat >"$OUT" <<EOF
{
  "scenario": "${SCENARIO}",
  "seed": ${SEED},
  "users": ${USERS},
  "ramp_seconds": ${RAMP_SECONDS},
  "target_login_rps": ${TARGET_LOGIN_RPS},
  "hold_seconds": ${HOLD_SECONDS},
  "map_template_id": ${MAP_TEMPLATE_ID},
  "spawn_distribution": "${SPAWN_DISTRIBUTION}",
  "move_hz": ${MOVE_HZ},
  "gateway_endpoints": "${GATEWAY_ENDPOINTS}",
  "chaos_enabled": ${CHAOS_ENABLED},
  "status": "${STATUS}",
  "reason": "${REASON}"
}
EOF
  echo "status=${STATUS} report=${OUT} reason=${REASON}"
}

if [[ "$GATEWAY_ENDPOINTS" == *124.222.244.169* ]]; then
  STATUS="BLOCKED"
  REASON="refusing public EIP"
  write_report
  exit 2
fi
if [[ -f "${ROOT}/run/formal/pids" ]]; then
  STATUS="BLOCKED"
  REASON="formal pid file present; this script will not press the formal cluster"
  write_report
  exit 2
fi
if [[ "$EXECUTE" -ne 1 ]]; then
  write_report
  exit 0
fi
if [[ "$HOST" != "127.0.0.1" && "$HOST" != "localhost" && "${GAMEMESH_STRESS_ALLOW_REMOTE:-}" != "1" ]]; then
  STATUS="BLOCKED"
  REASON="endpoint is not loopback"
  write_report
  exit 2
fi
if [[ "$USERS" -gt 20 && "${GAMEMESH_STRESS_ALLOW_LARGE:-}" != "1" ]]; then
  STATUS="BLOCKED"
  REASON="users>20 requires GAMEMESH_STRESS_ALLOW_LARGE=1 on an isolated host"
  write_report
  exit 2
fi
if [[ ! -x "$CLIENT" ]]; then
  STATUS="BLOCKED"
  REASON="missing ${CLIENT}"
  write_report
  exit 2
fi
if [[ "$CHAOS_ENABLED" -ne 0 ]]; then
  STATUS="NOT_RUN"
  REASON="chaos injection is not implemented; refusing to invent a fault run"
  write_report
  exit 2
fi

export PRESS_HOLD_MS=$((HOLD_SECONDS * 1000))
set +e
"$CLIENT" line-press "$HOST" "$PORT" "$MAP_TEMPLATE_ID" "$USERS" 0 >"${OUT}.client.log" 2>&1
RC=$?
set -e
if [[ "$RC" -eq 0 ]]; then
  STATUS="PASS"
  REASON="line-press exit 0; see ${OUT}.client.log. ramp/rps/move_hz are recorded only; line-press does not pace them"
else
  STATUS="FAIL"
  REASON="line-press exit ${RC}; see ${OUT}.client.log"
fi
write_report
exit "$RC"
