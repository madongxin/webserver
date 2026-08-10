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

log() { echo "== [stable_gate] $* =="; }
die() { echo "STABLE BLOCKED: $*" >&2; exit 1; }

log "check_deps --full"
./scripts/check_deps.sh --full || die "check_deps failed"

log "bootstrap_local_config"
./scripts/bootstrap_local_config.sh || die "bootstrap_local_config failed"

log "git diff --check (scripts/docs; exclude generated *.pb.*)"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  # protobuf 生成头常带尾随空白；门禁检查手写路径
  git diff --check -- scripts docs deploy .github CMakeLists.txt AGENTS.md \
    || die "trailing whitespace / conflict markers in scripts/docs"
else
  echo "WARN: not a git repo; skip git diff --check"
fi

log "shellcheck (if available)"
SC_BIN="$(command -v shellcheck || command -v ShellCheck || true)"
if [[ -n "$SC_BIN" ]]; then
  "$SC_BIN" -x scripts/stable_gate.sh scripts/install_deps.sh \
    scripts/test_e2e_20x.sh scripts/load_tcp_baseline.sh \
    scripts/soak_test.sh scripts/test_sanitizers.sh \
    deploy/docker-entrypoint.sh || die "shellcheck failed"
else
  echo "WARN: shellcheck not installed — --full will mark STABLE BLOCKED at end"
  SHELLCHECK_MISSING=1
fi

log "build Debug"
./scripts/build.sh Debug || die "Debug build failed"

if [[ "$FULL" -eq 1 ]]; then
  log "build Release"
  ./scripts/build.sh Release || die "Release build failed"
fi

log "unit"
./scripts/test.sh unit || die "unit failed"

log "integration"
./scripts/test.sh integration || die "integration failed"

if [[ "$WITH_E2E" -eq 1 ]]; then
  log "final_e2e --start-cluster"
  ./scripts/final_e2e.sh --start-cluster || die "final_e2e failed"
fi

if [[ "$FULL" -eq 1 ]]; then
  log "phase2 failover scripts"
  export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
  # 集群可能被 final_e2e 杀残；必要时重启
  if [[ ! -f "$GAMEMESH_RUN_DIR/pids" ]]; then
    ./scripts/run_e2e_cluster.sh
  fi
  ./scripts/test_channel_snapshot_race.sh || die "channel snapshot race"
  ./scripts/test_registry_outage.sh || die "registry outage"
  # session failover 会杀 session0 — 单独跑并重启集群
  ./scripts/test_session_failover.sh || die "session failover"
  ./scripts/stop_e2e_cluster.sh 2>/dev/null || true
  sleep 1
  ./scripts/run_e2e_cluster.sh
  export GAMEMESH_RUN_DIR="$ROOT/run/e2e"
  # shellcheck disable=SC1090
  source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
  ./scripts/test_logic_auto_recovery.sh || die "logic auto recovery"
  ./scripts/stop_e2e_cluster.sh 2>/dev/null || true
  sleep 1
  ./scripts/run_e2e_cluster.sh
  # shellcheck disable=SC1090
  source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
  ./scripts/test_gamedb_unknown_result_failover.sh || die "gamedb failover"
  ./scripts/test_dynamic_logic_scale.sh || die "dynamic logic scale"

  log "sanitizers"
  ./scripts/test_sanitizers.sh all || die "sanitizers failed"

  log "e2e ${E2E_ROUNDS:-20}x"
  START_CLUSTER=1 E2E_ROUNDS="${E2E_ROUNDS:-20}" ./scripts/test_e2e_20x.sh || die "e2e 20x failed"

  log "load baseline ${LOAD_DURATION_SEC:-1800}s"
  ./scripts/load_tcp_baseline.sh || die "load baseline failed"

  log "soak ${SOAK_DURATION_SEC:-7200}s (or SOAK_REPORT)"
  if [[ -n "${SOAK_REPORT:-}" ]]; then
    ./scripts/soak_test.sh --verify-only || die "soak report invalid"
  else
    ./scripts/soak_test.sh || die "soak failed"
  fi
fi

log "PASS (e2e=$WITH_E2E full=$FULL)"
if [[ "$FULL" -eq 1 ]]; then
  blocked=0
  if [[ "${SHELLCHECK_MISSING:-0}" -eq 1 ]]; then
    echo "STABLE BLOCKED: shellcheck missing"; blocked=1
  fi
  if [[ "${SOAK_DURATION_SEC:-7200}" -lt 7200 && -z "${SOAK_REPORT:-}" ]]; then
    echo "STABLE BLOCKED: soak duration ${SOAK_DURATION_SEC:-7200}s < 7200 and no SOAK_REPORT"
    blocked=1
  fi
  if [[ "${LOAD_DURATION_SEC:-1800}" -lt 1800 ]]; then
    echo "STABLE BLOCKED: load duration ${LOAD_DURATION_SEC}s < 1800"
    blocked=1
  fi
  if [[ "${E2E_ROUNDS:-20}" -lt 20 ]]; then
    echo "STABLE BLOCKED: E2E_ROUNDS ${E2E_ROUNDS} < 20"
    blocked=1
  fi
  if [[ "$blocked" -ne 0 ]]; then
    echo "Re-run with full durations or SOAK_REPORT for this commit."
    exit 1
  fi
  echo "STABLE PASS (candidate server-stable-v0.1.0-rc1) — still requires human review before tag"
else
  echo "NOTE: not --full; stable判定仍为 BLOCKED。完整门禁: ./scripts/stable_gate.sh --full"
fi
