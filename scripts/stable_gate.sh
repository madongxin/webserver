#!/usr/bin/env bash
# 工作包一稳定版本门禁（缺依赖/缺二进制必须非零退出；禁止伪 SKIP 当成功）
# 用法:
#   ./scripts/stable_gate.sh              # check_deps + bootstrap + Debug + unit + integration
#   ./scripts/stable_gate.sh --with-e2e   # 另启集群跑 final_e2e
#   ./scripts/stable_gate.sh --full       # 含 Release 构建（耗时）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

WITH_E2E=0
WITH_RELEASE=0
for a in "$@"; do
  case "$a" in
    --with-e2e) WITH_E2E=1 ;;
    --full) WITH_RELEASE=1; WITH_E2E=1 ;;
    *)
      echo "usage: $0 [--with-e2e] [--full]" >&2
      exit 2
      ;;
  esac
done

log() { echo "== [stable_gate] $* =="; }

log "check_deps --full"
./scripts/check_deps.sh --full

log "bootstrap_local_config"
./scripts/bootstrap_local_config.sh

log "build Debug"
./scripts/build.sh Debug

if [[ "$WITH_RELEASE" -eq 1 ]]; then
  log "build Release"
  ./scripts/build.sh Release
fi

log "unit"
./scripts/test.sh unit

log "integration"
./scripts/test.sh integration

if [[ "$WITH_E2E" -eq 1 ]]; then
  log "final_e2e --start-cluster"
  ./scripts/final_e2e.sh --start-cluster
fi

log "PASS (e2e=$WITH_E2E release=$WITH_RELEASE)"
echo "NOTE: ASan/UBSan/TSan、20×E2E、soak 需另跑；未跑则稳定判定仍为 BLOCKED"
