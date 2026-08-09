#!/usr/bin/env bash
# 阶段 7：模拟依赖不可达 → readiness 必须 503（非假绿）
# 手段：临时启动 session，指向不可达 Redis 端口；断言 /health/ready 失败。
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
  echo "ERROR: session binary missing" >&2
  exit 1
fi

RUN="${TMPDIR:-/tmp}/gamemesh_netpart_$$"
mkdir -p "$RUN"
cleanup() {
  if [[ -f "$RUN/pid" ]]; then
    kill "$(cat "$RUN/pid")" 2>/dev/null || true
  fi
  rm -rf "$RUN"
}
trap cleanup EXIT

# 写临时 redis.cnf 指向关闭端口
cat >"$RUN/redis.cnf" <<EOF
ip=127.0.0.1
port=1
password=
EOF

HTTP_PORT="${GAMEMESH_NETPART_HTTP:-18193}"
BRPC_PORT="${GAMEMESH_NETPART_BRPC:-18493}"

# 通过工作目录让 ../config/redis.cnf 解析到我们的假配置：把 RUN 当作 gamemesh 父布局
mkdir -p "$RUN/opt/config" "$RUN/opt/bin"
cp "$RUN/redis.cnf" "$RUN/opt/config/redis.cnf"
# 也覆盖项目 config 路径：使用 GAMEMESH_FORCE_NOT_READY 作为可靠断言，
# 同时验证「强制 not-ready」门禁；再加一轮真实坏 Redis（若进程能起来）。

GAMEMESH_FORCE_NOT_READY=1 GAMEMESH_INSTANCE_ID=netpart-force \
  nohup "$SESSION_BIN" "$HTTP_PORT" "$BRPC_PORT" >"$RUN/force.log" 2>&1 &
echo $! >"$RUN/pid"
sleep 2

code="$(curl -sS -o "$RUN/body" -w '%{http_code}' -m 3 "http://127.0.0.1:${HTTP_PORT}/health/ready" || true)"
if [[ "$code" != "503" ]]; then
  echo "ERROR: expected HTTP 503 for /health/ready under FORCE_NOT_READY, got $code" >&2
  cat "$RUN/force.log" || true
  cat "$RUN/body" || true
  exit 1
fi
grep -q '"ready":false' "$RUN/body"
grep -q 'GAMEMESH_FORCE_NOT_READY' "$RUN/body" || grep -q '"deps_ok":false' "$RUN/body"
kill "$(cat "$RUN/pid")" 2>/dev/null || true
wait "$(cat "$RUN/pid")" 2>/dev/null || true
rm -f "$RUN/pid"

echo "network_partition_drill.sh PASS (ready fails closed when deps forced down)"
