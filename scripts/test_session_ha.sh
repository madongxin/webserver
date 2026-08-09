#!/usr/bin/env bash
# 阶段 3：双 Session HA 演练（杀一实例后另一实例仍可响应 Auth/Session brpc）
# 依赖：已 build；Redis；可绑定 8401/8402 与临时 HTTP 口
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SESSION_BIN=""
if [[ -x "$ROOT/build/test/session" ]]; then
  SESSION_BIN="$ROOT/build/test/session"
elif [[ -x "$ROOT/build/session" ]]; then
  SESSION_BIN="$ROOT/build/session"
fi
if [[ -z "$SESSION_BIN" ]]; then
  echo "ERROR: session binary missing; build first" >&2
  exit 1
fi

RUN="${TMPDIR:-/tmp}/gamemesh_session_ha_$$"
mkdir -p "$RUN"
cleanup() {
  if [[ -f "$RUN/pids" ]]; then
    while read -r p; do kill "$p" 2>/dev/null || true; done <"$RUN/pids"
  fi
  rm -rf "$RUN"
}
trap cleanup EXIT

S0_PORT="${GAMEMESH_HA_SESSION0:-18401}"
S1_PORT="${GAMEMESH_HA_SESSION1:-18402}"
H0="${GAMEMESH_HA_HTTP0:-18093}"
H1="${GAMEMESH_HA_HTTP1:-18096}"

: >"$RUN/pids"
start_one() {
  local id="$1" http="$2" brpc="$3" log="$4"
  if [[ "$(basename "$SESSION_BIN")" == "server" ]]; then
    GAMEMESH_INSTANCE_ID="$id" nohup "$SESSION_BIN" session "$http" "$brpc" >"$log" 2>&1 &
  else
    GAMEMESH_INSTANCE_ID="$id" nohup "$SESSION_BIN" "$http" "$brpc" >"$log" 2>&1 &
  fi
  echo $! >>"$RUN/pids"
}

start_one sess-ha-0 "$H0" "$S0_PORT" "$RUN/s0.log"
start_one sess-ha-1 "$H1" "$S1_PORT" "$RUN/s1.log"

ready() {
  local f="$1" t=0
  while (( t < 60 )); do
    if grep -qE 'SessionBrpcServer|role=session' "$f" 2>/dev/null; then
      return 0
    fi
    sleep 0.5
    t=$((t + 1))
  done
  return 1
}

ready "$RUN/s0.log" || { echo "ERROR s0 not ready"; cat "$RUN/s0.log"; exit 1; }
ready "$RUN/s1.log" || { echo "ERROR s1 not ready"; cat "$RUN/s1.log"; exit 1; }

# 健康：HTTP /api/version
curl -fsS "http://127.0.0.1:${H0}/api/version" | grep -q session
curl -fsS "http://127.0.0.1:${H1}/api/version" | grep -q session
echo "both sessions healthy"

# 杀 sess-0，sess-1 仍 ready
kill "$(sed -n '1p' "$RUN/pids")" 2>/dev/null || true
sleep 1
if curl -fsS "http://127.0.0.1:${H0}/api/version" >/dev/null 2>&1; then
  echo "WARN: killed session still answering HTTP (slow die?)"
fi
curl -fsS "http://127.0.0.1:${H1}/api/version" | grep -q session
echo "survivor session still ready after kill"

# Gateway 配置应能解析为双地址（命名工具契约由 discovery_ha_test 覆盖）
echo "test_session_ha.sh PASS (kill-one survivor OK; client list:// covered by unit + cluster cnf)"
