#!/usr/bin/env bash
# 稳定版本硬门禁：任何必需步骤失败或未执行 → 非零 + STABLE BLOCKED
# 用法:
#   ./scripts/stable_gate.sh              # Debug + unit + integration
#   ./scripts/stable_gate.sh --with-e2e
#   ./scripts/stable_gate.sh --full       # 完整稳定门禁（耗时）
#
# --full 可选加速（仅开发冒烟，不算正式稳定）:
#   LOAD_DURATION_SEC=60 SOAK_DURATION_SEC=120 E2E_ROUNDS=3 ./scripts/stable_gate.sh --full
# 正式发布请使用默认：20 轮 E2E、30min 负载、2h soak（或 SOAK_REPORT=同 commit 报告）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

WITH_E2E=0
FULL=0
SHELLCHECK_MISSING=0
for a in "$@"; do
  case "$a" in
    --with-e2e) WITH_E2E=1 ;;
    --full) FULL=1; WITH_E2E=1 ;;
    *)
      echo "usage: $0 [--with-e2e] [--full]" >&2
      exit 2
      ;;
  esac
done

GATE_START_EPOCH="$(date +%s)"
GATE_START_ISO="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
GATE_COMMIT="nogit"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  GATE_COMMIT="$(git rev-parse HEAD 2>/dev/null || echo nogit)"
fi
SUMMARY_DIR="${ROOT}/run/stable_gate"
mkdir -p "$SUMMARY_DIR/failover" "$SUMMARY_DIR/sanitizers"
SUMMARY_JSON="${SUMMARY_DIR}/summary_${GATE_START_EPOCH}.json"
SUMMARY_STEPS=()

log() { echo "== [stable_gate] $* =="; }
die() {
  echo "STABLE BLOCKED: $*" >&2
  write_summary 1 "$*"
  exit 1
}

run_step() {
  local name="$1"
  shift
  log "$name"
  set +e
  if [[ $# -ge 1 && "$1" == "--log" ]]; then
    local logfile="$2"
    shift 2
    "$@" >"$logfile" 2>&1
  else
    "$@"
  fi
  local rc=$?
  set -e
  SUMMARY_STEPS+=("{\"name\":\"${name}\",\"exit_code\":${rc}}")
  [[ "$rc" -eq 0 ]] || die "$name failed (rc=$rc)"
}

write_summary() {
  local final_rc="${1:-0}" reason="${2:-}"
  local end_epoch end_iso steps_csv
  end_epoch="$(date +%s)"
  end_iso="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  steps_csv=""
  local s
  for s in "${SUMMARY_STEPS[@]:-}"; do
    [[ -n "$steps_csv" ]] && steps_csv+=","
    steps_csv+="$s"
  done
  cat >"$SUMMARY_JSON" <<EOF
{
  "commit": "${GATE_COMMIT}",
  "start_time": "${GATE_START_ISO}",
  "end_time": "${end_iso}",
  "start_epoch": ${GATE_START_EPOCH},
  "end_epoch": ${end_epoch},
  "with_e2e": ${WITH_E2E},
  "full": ${FULL},
  "exit_code": ${final_rc},
  "blocked_reason": $(printf '%s' "$reason" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))' 2>/dev/null || echo "\"$reason\""),
  "report_path": "${SUMMARY_JSON}",
  "steps": [${steps_csv}]
}
EOF
  echo "stable_gate summary: $SUMMARY_JSON"
  # 导出不可手工伪造的 release 目录
  local verdict="STABLE BLOCKED"
  if [[ "$final_rc" -eq 0 && "$FULL" -eq 1 ]]; then
    verdict="STABLE PASS — candidate server-stable-v0.1.0-rc1"
  fi
  export STABLE_VERDICT="$verdict"
  export STABLE_EXIT_CODE="$final_rc"
  export STABLE_STEPS_JSON="[${steps_csv}]"
  "$ROOT/scripts/export_release_bundle.sh" "$GATE_COMMIT" || true
}

# 破坏性场景后强制健康检查；不健康则 stop+restart
ensure_e2e_ready() {
  export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
  e2e_ensure_cluster "$ROOT" || return 1
  # shellcheck disable=SC1090
  [[ -f "$GAMEMESH_RUN_DIR/E2E_PORTS.env" ]] && source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
  return 0
}

restart_e2e_clean() {
  export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
  "$ROOT/scripts/stop_e2e_cluster.sh" 2>/dev/null || true
  sleep 1
  rm -f "$GAMEMESH_RUN_DIR/pids" "$GAMEMESH_RUN_DIR/inventory.tsv"
  "$ROOT/scripts/run_e2e_cluster.sh" || return 1
  sleep 2
  # shellcheck disable=SC1090
  source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
  e2e_cluster_healthy || return 1
}

run_step "check_deps --full" ./scripts/check_deps.sh --full
run_step "bootstrap_local_config" ./scripts/bootstrap_local_config.sh

log "git diff --check (scripts/docs; exclude generated *.pb.*)"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  if git diff --check -- scripts docs deploy .github CMakeLists.txt AGENTS.md apps game db runtime \
      ':(exclude)*.pb.cc' ':(exclude)*.pb.h'; then
    SUMMARY_STEPS+=("{\"name\":\"git_diff_check\",\"exit_code\":0}")
  else
    SUMMARY_STEPS+=("{\"name\":\"git_diff_check\",\"exit_code\":1}")
    die "trailing whitespace / conflict markers in scripts/docs"
  fi
  log "bash -n scripts"
  if bash -n scripts/*.sh && bash -n deploy/docker-entrypoint.sh; then
    SUMMARY_STEPS+=("{\"name\":\"bash_n\",\"exit_code\":0}")
  else
    SUMMARY_STEPS+=("{\"name\":\"bash_n\",\"exit_code\":1}")
    die "bash -n failed"
  fi
else
  echo "WARN: not a git repo; skip git diff --check"
  SUMMARY_STEPS+=("{\"name\":\"git_diff_check\",\"exit_code\":0}")
fi

log "shellcheck (if available)"
SC_BIN="$(command -v shellcheck || command -v ShellCheck || true)"
if [[ -n "$SC_BIN" ]]; then
  if "$SC_BIN" -x scripts/stable_gate.sh scripts/install_deps.sh \
    scripts/test_e2e_20x.sh scripts/load_tcp_baseline.sh \
    scripts/soak_test.sh scripts/test_sanitizers.sh \
    scripts/e2e_inventory.sh scripts/export_release_bundle.sh \
    deploy/docker-entrypoint.sh; then
    SUMMARY_STEPS+=("{\"name\":\"shellcheck\",\"exit_code\":0}")
  else
    SUMMARY_STEPS+=("{\"name\":\"shellcheck\",\"exit_code\":1}")
    die "shellcheck failed"
  fi
else
  echo "WARN: shellcheck not installed — --full will mark STABLE BLOCKED at end"
  SHELLCHECK_MISSING=1
  SUMMARY_STEPS+=("{\"name\":\"shellcheck\",\"exit_code\":0}")
fi

run_step "build Debug" --log "$SUMMARY_DIR/build-debug.log" ./scripts/build.sh Debug

if [[ "$FULL" -eq 1 ]]; then
  run_step "build Release" --log "$SUMMARY_DIR/build-release.log" ./scripts/build.sh Release
fi

run_step "unit" --log "$SUMMARY_DIR/unit.log" ./scripts/test.sh unit
run_step "integration" --log "$SUMMARY_DIR/integration.log" ./scripts/test.sh integration

if [[ "$WITH_E2E" -eq 1 ]]; then
  run_step "final_e2e --start-cluster" ./scripts/final_e2e.sh --start-cluster
fi

if [[ "$FULL" -eq 1 ]]; then
  log "phase2 failover scripts (restart topology after each destructive group)"
  export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
  export GAMEMESH_REQUIRE_BRPC_TSAN=1

  run_step "ensure_e2e_after_final_e2e" ensure_e2e_ready
  run_step "channel_snapshot_race" --log "$SUMMARY_DIR/failover/channel_snapshot_race.log" \
    ./scripts/test_channel_snapshot_race.sh

  run_step "registry_outage" --log "$SUMMARY_DIR/failover/registry_outage.log" \
    ./scripts/test_registry_outage.sh
  run_step "restart_after_registry" restart_e2e_clean

  run_step "session_failover" --log "$SUMMARY_DIR/failover/session_failover.log" \
    ./scripts/test_session_failover.sh
  run_step "restart_after_session" restart_e2e_clean

  run_step "logic_auto_recovery" --log "$SUMMARY_DIR/failover/logic_auto_recovery.log" \
    ./scripts/test_logic_auto_recovery.sh
  run_step "restart_after_logic" restart_e2e_clean

  run_step "gamedb_unknown_failover" --log "$SUMMARY_DIR/failover/gamedb_unknown_failover.log" \
    ./scripts/test_gamedb_unknown_result_failover.sh
  run_step "restart_after_gamedb" restart_e2e_clean

  run_step "dynamic_logic_scale" --log "$SUMMARY_DIR/failover/dynamic_logic_scale.log" \
    ./scripts/test_dynamic_logic_scale.sh
  run_step "restart_after_scale" restart_e2e_clean

  run_step "sanitizers" --log "$SUMMARY_DIR/sanitizers/all.log" ./scripts/test_sanitizers.sh all
  run_step "e2e_${E2E_ROUNDS:-20}x" --log "$SUMMARY_DIR/e2e-20x.log" \
    env START_CLUSTER=1 E2E_ROUNDS="${E2E_ROUNDS:-20}" ./scripts/test_e2e_20x.sh
  run_step "load_baseline" ./scripts/load_tcp_baseline.sh

  if [[ -n "${SOAK_REPORT:-}" ]]; then
    run_step "soak_verify" ./scripts/soak_test.sh --verify-only
  else
    run_step "soak" ./scripts/soak_test.sh
  fi
fi

log "PASS (e2e=$WITH_E2E full=$FULL)"
if [[ "$FULL" -eq 1 ]]; then
  blocked=0
  reason=""
  if [[ "${SHELLCHECK_MISSING:-0}" -eq 1 ]]; then
    echo "STABLE BLOCKED: shellcheck missing"; blocked=1
    reason="shellcheck missing"
  fi
  if [[ "${SOAK_DURATION_SEC:-7200}" -lt 7200 && -z "${SOAK_REPORT:-}" ]]; then
    echo "STABLE BLOCKED: soak duration ${SOAK_DURATION_SEC:-7200}s < 7200 and no SOAK_REPORT"
    blocked=1
    reason="soak duration short"
  fi
  if [[ "${LOAD_DURATION_SEC:-1800}" -lt 1800 ]]; then
    echo "STABLE BLOCKED: load duration ${LOAD_DURATION_SEC}s < 1800"
    blocked=1
    reason="load duration short"
  fi
  if [[ "${E2E_ROUNDS:-20}" -lt 20 ]]; then
    echo "STABLE BLOCKED: E2E_ROUNDS ${E2E_ROUNDS} < 20"
    blocked=1
    reason="e2e rounds short"
  fi
  if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
      echo "NOTE: working tree dirty — manifest will mark dirty=true"
    fi
  fi
  if [[ "$blocked" -ne 0 ]]; then
    write_summary 1 "$reason"
    echo "Re-run with full durations or SOAK_REPORT for this commit."
    echo "STABLE BLOCKED"
    exit 1
  fi
  write_summary 0 ""
  echo "STABLE PASS — candidate server-stable-v0.1.0-rc1"
  echo "(still requires human review before tag/push)"
else
  write_summary 0 "not --full"
  echo "STABLE BLOCKED"
  echo "NOTE: not --full; 完整门禁: ./scripts/stable_gate.sh --full"
fi
