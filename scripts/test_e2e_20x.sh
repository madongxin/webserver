#!/usr/bin/env bash
# 核心 E2E 连续 N 轮（默认 20）
# 用法: ./scripts/test_e2e_20x.sh [N]
# 需已 build；每轮 --start-cluster 过重，默认复用集群（首轮可 START_CLUSTER=1）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
N="${1:-${E2E_ROUNDS:-20}}"
if [[ ! "$N" =~ ^[0-9]+$ ]] || (( N < 1 )); then
  echo "ERROR: bad rounds"; exit 2
fi

if [[ "${START_CLUSTER:-0}" == "1" ]]; then
  export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
  if [[ -f "$GAMEMESH_RUN_DIR/pids" ]]; then
    ./scripts/stop_e2e_cluster.sh 2>/dev/null || true
    sleep 1
  fi
  ./scripts/run_e2e_cluster.sh
  sleep 2
  # 冷启动：等 Register/Login 真正可用（避免 Auth RR / GameDB 通道未热）
  CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
  # shellcheck disable=SC1090
  [[ -f "$GAMEMESH_RUN_DIR/E2E_PORTS.env" ]] && source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
  GW="${E2E_GW0_GAME:-19081}"
  ready=0
  for _ in $(seq 1 60); do
    if [[ -x "$CLIENT" ]] && "$CLIENT" register-login 127.0.0.1 "$GW" "e2e_warm_$$_$_" e2epass1 2>/dev/null | grep -q login_ok=1; then
      ready=1
      break
    fi
    sleep 1
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "ERROR: cluster started but register-login not ready within 60s" >&2
    exit 1
  fi
elif [[ ! -f "${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}/pids" ]]; then
  export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
  ./scripts/run_e2e_cluster.sh
  sleep 2
fi
export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
if [[ -f "$GAMEMESH_RUN_DIR/E2E_PORTS.env" ]]; then
  # shellcheck disable=SC1090
  source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
fi

fail=0
for i in $(seq 1 "$N"); do
  echo "==== e2e round $i/$N ===="
  # 每轮跑稳定场景子集，避免杀进程拖垮后续轮
  if ! SCENARIOS="${E2E_SCENARIOS:-1,2,3}" ./scripts/test_final_e2e.sh; then
    echo "ERROR: e2e round $i failed" >&2
    fail=1
    break
  fi
done
if [[ "$fail" -ne 0 ]]; then
  echo "test_e2e_20x.sh FAIL at round <=$i"
  exit 1
fi
echo "test_e2e_20x.sh PASS rounds=$N"
