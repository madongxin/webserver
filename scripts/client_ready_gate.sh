#!/usr/bin/env bash
# 客户端就绪门禁：公网 Gateway TCP，结构化 key=value / PASS 行，禁止只 grep 一条日志。
# 顺序：Hello → 契约/进图 → AOI → 容量 → 邮件 → 社交 → 世界快照（含 Logic 重启）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/unity-e2e}"
export GAMEMESH_MAP_SHA256_FILE="${GAMEMESH_MAP_SHA256_FILE:-$ROOT/config/maps/map_1001.json.sha256}"
export GAMEMESH_MAP_DATA_VERSION="${GAMEMESH_MAP_DATA_VERSION:-1}"
export GAMEMESH_SKIP_CLUSTER_STOP=1
# 完整 CLIENT READY 必须核对 Unity；CI 无 luna 时用 GAMEMESH_CI_TCP_ONLY=1，只宣称 CLIENT TCP PASS。
CI_TCP_ONLY="${GAMEMESH_CI_TCP_ONLY:-0}"
if [[ "$CI_TCP_ONLY" == "1" ]]; then
  export GAMEMESH_REQUIRE_LUNA_CONTRACT=0
else
  export GAMEMESH_REQUIRE_LUNA_CONTRACT=1
fi

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: missing $CLIENT (./scripts/build.sh Debug)" >&2; exit 1; }

GATE_START="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
SUMMARY_DIR="${ROOT}/run/client_ready"
mkdir -p "$SUMMARY_DIR"
SUMMARY="${SUMMARY_DIR}/summary_$(date +%s).json"
STEPS=()

die() {
  echo "CLIENT READY BLOCKED: $*" >&2
  write_summary 1 "$*"
  exit 1
}

write_summary() {
  local rc="${1:-1}" reason="${2:-}"
  local csv="" s
  for s in "${STEPS[@]:-}"; do
    [[ -n "$csv" ]] && csv+=","
    csv+="$s"
  done
  cat >"$SUMMARY" <<EOF
{
  "gate": "client_ready",
  "start_time": "${GATE_START}",
  "end_time": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "run_dir": "${GAMEMESH_RUN_DIR}",
  "exit_code": ${rc},
  "blocked_reason": $(printf '%s' "$reason" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))' 2>/dev/null || echo "\"$reason\""),
  "steps": [${csv}]
}
EOF
  echo "client_ready summary: $SUMMARY"
}

run_tcp() {
  local name="$1"
  local script="$2"
  echo "== client_ready: $name =="
  e2e_ensure_cluster "$ROOT" || die "$name cluster unhealthy"
  # shellcheck disable=SC1090
  [[ -f "$GAMEMESH_RUN_DIR/E2E_PORTS.env" ]] && source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
  local log rc
  log="${SUMMARY_DIR}/${name}.log"
  set +e
  "$script" >"$log" 2>&1
  rc=$?
  set -e
  cat "$log"
  STEPS+=("{\"name\":\"${name}\",\"exit_code\":${rc},\"log\":\"${log}\"}")
  [[ "$rc" -eq 0 ]] || die "$name failed rc=$rc (see $log)"
  grep -q "PASS" "$log" || die "$name missing PASS token in structured output"
}

echo "== client_ready: luna_protocol_contract =="
set +e
luna_out="$("$ROOT/scripts/check_luna_protocol_contract.sh" 2>&1)"
luna_rc=$?
set -e
echo "$luna_out"
STEPS+=("{\"name\":\"luna_protocol_contract\",\"exit_code\":${luna_rc}}")
if [[ "$CI_TCP_ONLY" == "1" ]]; then
  if [[ "$luna_rc" -ne 0 ]]; then
    die "luna protocol contract failed rc=$luna_rc"
  fi
  if echo "$luna_out" | grep -q "luna_protocol_contract=NOT_RUN"; then
    echo "INFO: luna contract NOT RUN (GAMEMESH_CI_TCP_ONLY=1); not CLIENT READY"
  elif ! echo "$luna_out" | grep -q "luna_protocol_contract=PASS"; then
    die "luna protocol contract missing PASS/NOT_RUN token"
  fi
else
  [[ "$luna_rc" -eq 0 ]] || die "luna protocol contract failed rc=$luna_rc"
  echo "$luna_out" | grep -q "luna_protocol_contract=PASS" \
    || die "luna protocol contract missing PASS token"
fi

run_tcp hello_heartbeat "$ROOT/scripts/test_hello_heartbeat.sh"
run_tcp unity_contract "$ROOT/scripts/test_unity_contract.sh"
run_tcp two_player_aoi "$ROOT/scripts/test_two_player_aoi.sh"
run_tcp map_capacity "$ROOT/scripts/test_map_capacity.sh"
run_tcp player_mail "$ROOT/scripts/test_player_mail_e2e.sh"
run_tcp s3_social "$ROOT/scripts/test_s3_social.sh"
run_tcp world_snapshot "$ROOT/scripts/test_world_snapshot.sh"

write_summary 0 ""
if [[ "$CI_TCP_ONLY" == "1" ]]; then
  echo "CLIENT TCP PASS"
  echo "client_ready_gate.sh TCP PASS (not CLIENT READY; luna contract not required)"
else
  echo "CLIENT READY PASS"
  echo "client_ready_gate.sh PASS"
fi
