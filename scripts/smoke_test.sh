#!/usr/bin/env bash
# 多角色健康冒烟：校验 /api/version 中的 service 字段，避免只看容器 running。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

HOST="${GAMEMESH_SMOKE_HOST:-127.0.0.1}"
fail=0

# port:expected_service
TARGETS=(
  "${GAMEMESH_SMOKE_GW0:-8080}:gateway"
  "${GAMEMESH_SMOKE_GW1:-8082}:gateway"
  "${GAMEMESH_SMOKE_LOGIC0:-8090}:gamelogic"
  "${GAMEMESH_SMOKE_LOGIC1:-8091}:gamelogic"
  "${GAMEMESH_SMOKE_WORLD:-8092}:world"
  "${GAMEMESH_SMOKE_SESSION0:-8093}:session"
  "${GAMEMESH_SMOKE_GAMEDB0:-8094}:gamedb"
  "${GAMEMESH_SMOKE_SESSION1:-8096}:session"
)

check_role() {
  local port="$1"
  local expect="$2"
  local url="http://${HOST}:${port}/api/version"
  local body
  if ! body="$(curl -fsS -m 3 "$url" 2>/dev/null)"; then
    echo "FAIL $url (unreachable)"
    fail=1
    return
  fi
  if ! grep -q "\"service\":\"${expect}\"" <<<"$body"; then
    echo "FAIL $url expected service=${expect} got: ${body:0:160}"
    fail=1
    return
  fi
  local live
  live="$(curl -fsS -m 3 "http://${HOST}:${port}/health/live" 2>/dev/null || true)"
  if ! grep -q '"alive":true' <<<"$live"; then
    echo "FAIL /health/live port=${port}: ${live:0:120}"
    fail=1
    return
  fi
  local ready_code ready_body
  ready_code="$(curl -sS -o /tmp/gm_ready_$$.json -w '%{http_code}' -m 3 \
    "http://${HOST}:${port}/health/ready" 2>/dev/null || echo 000)"
  ready_body="$(cat /tmp/gm_ready_$$.json 2>/dev/null || true)"
  rm -f /tmp/gm_ready_$$.json
  if [[ "$ready_code" != "200" ]] || ! grep -q '"ready":true' <<<"$ready_body"; then
    echo "FAIL /health/ready port=${port} code=${ready_code} body=${ready_body:0:160}"
    fail=1
    return
  fi
  echo "ok ${expect}@${port} ${body:0:100}"
}

for t in "${TARGETS[@]}"; do
  check_role "${t%%:*}" "${t##*:}"
done

# 兼容旧入口：单端口冒烟
if [[ -n "${GAMEMESH_SMOKE_HTTP:-}" ]]; then
  check_role "$GAMEMESH_SMOKE_HTTP" "${GAMEMESH_SMOKE_EXPECT_SERVICE:-gateway}"
fi

if [[ "$fail" -ne 0 ]]; then
  echo "smoke_test.sh FAIL"
  exit 1
fi
echo "smoke_test.sh PASS"
