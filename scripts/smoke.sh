#!/usr/bin/env bash
# 冒烟：健康检查 + 可选版本接口（需集群已启动）
set -euo pipefail
HOST="${GAMEMESH_SMOKE_HOST:-127.0.0.1}"
PORT="${GAMEMESH_SMOKE_HTTP:-8080}"
fail=0
check() {
  local url="$1"
  if ! curl -fsS -m 2 "$url" >/tmp/gamemesh_smoke.json; then
    echo "FAIL $url"
    fail=1
    return
  fi
  echo "ok $url $(head -c 120 /tmp/gamemesh_smoke.json)"
}
check "http://${HOST}:${PORT}/api/liveness"
check "http://${HOST}:${PORT}/api/version"
if [[ "$fail" -ne 0 ]]; then
  echo "smoke.sh FAIL (is cluster up? HTTP bind may be 127.0.0.1 under FORMAL)"
  exit 1
fi
echo "smoke.sh PASS"
