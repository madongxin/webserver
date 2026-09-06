#!/usr/bin/env bash
# 1002 当苏州 LINE：系统选线压 N 人（默认 1000）进同一条软顶线。
# 必须先滚动正式集群（新 catalog + GAMEMESH_CONNECT_RATE_MAX）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
ulimit -n 65535 || true
unset GAMEMESH_MAP_SHA256 GAMEMESH_MAP_SHA256_FILE

N="${1:-3000}"
HOST="${2:-127.0.0.1}"
PORT="${3:-8081}"
CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: missing $CLIENT" >&2; exit 1; }

python3 - "$ROOT/config/redis.cnf" <<'PY'
import os, subprocess, sys
cnf = sys.argv[1]
kv = {}
if os.path.isfile(cnf):
    for line in open(cnf):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        kv[k.strip()] = v.strip()
host = os.environ.get("GAMEMESH_REDIS_HOST", kv.get("ip", "127.0.0.1"))
port = os.environ.get("GAMEMESH_REDIS_PORT", kv.get("port", "6379"))
pw = os.environ.get("GAMEMESH_REDIS_PASSWORD", kv.get("password", ""))
prefix = kv.get("key_prefix", "gamemesh:dev")
if not prefix.endswith(":"):
    prefix += ":"
cmd = ["redis-cli", "-h", host, "-p", str(port)]
if pw:
    cmd += ["-a", pw, "--no-auth-warning"]
keys = [
    prefix + "map:pool:1:1002",
    prefix + "map:tpl:1:1002",
    prefix + "map:lines:1:1002",
]
# 线号映射 + 该模板实例（SCAN，避免误删 1001）
scan = cmd + ["--scan", "--pattern", prefix + "map:line:1:1002:*"]
p = subprocess.run(scan, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
extra = [x for x in (p.stdout or "").splitlines() if x]
# 旧 1002 LEGACY 房：从 pool members 再删 inst/occ
pool = prefix + "map:pool:1:1002"
ids_out = subprocess.run(cmd + ["ZRANGE", pool, "0", "-1"], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, universal_newlines=True)
for mid in (ids_out.stdout or "").split():
    extra += [prefix + "map:inst:" + mid, prefix + "map:occ:" + mid]
lines = prefix + "map:lines:1:1002"
lids = subprocess.run(cmd + ["ZRANGE", lines, "0", "-1"], stdout=subprocess.PIPE,
                      stderr=subprocess.PIPE, universal_newlines=True)
for mid in (lids.stdout or "").split():
    extra += [prefix + "map:inst:" + mid, prefix + "map:occ:" + mid,
              prefix + "map:members:" + mid]
all_keys = keys + extra
if all_keys:
    subprocess.run(cmd + ["DEL"] + all_keys, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("flushed 1002 placement keys", len(all_keys))
PY

echo "== line-press 1002 n=${N} ${HOST}:${PORT} =="
"$CLIENT" line-press "$HOST" "$PORT" 1002 "$N" 0
