#!/usr/bin/env bash
# 公共地图 50 人满员后第 51 人进入新实例；分布必须是 [50,1]。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"

CLIENT="${ROOT}/build/test/game_tcp_e2e_client"
[[ -x "$CLIENT" ]] || { echo "ERROR: missing $CLIENT (./scripts/build.sh Debug)" >&2; exit 1; }

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/unity-e2e}"
export GAMEMESH_MAP_SHA256_FILE="${GAMEMESH_MAP_SHA256_FILE:-$ROOT/config/maps/map_1001.json.sha256}"
export GAMEMESH_MAP_DATA_VERSION="${GAMEMESH_MAP_DATA_VERSION:-1}"

# 清掉本测试模板的公共池，避免历史 occupancy 把 51 人拆成 45+6。
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
key = prefix + "map:pool:1:1001"
cmd = ["redis-cli", "-h", host, "-p", str(port)]
if pw:
    cmd += ["-a", pw, "--no-auth-warning"]
cmd += ["DEL", key]
p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
if p.returncode != 0:
    sys.stderr.write(p.stderr or p.stdout or "redis-cli DEL failed\n")
    sys.exit(1)
print("flushed", key, "deleted", (p.stdout or "").strip())
PY

e2e_ensure_cluster "$ROOT"
# shellcheck disable=SC1090
source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:?}"
e2e_wait_login "$CLIENT" "$HOST" "$GW0"

set +e
out="$("$CLIENT" map-capacity-51 "$HOST" "$GW0" 1001 51)"
rc=$?
set -e
echo "$out"
[[ "$rc" -eq 0 ]] || { echo "ERROR: map-capacity-51 rc=$rc" >&2; exit "$rc"; }
echo "$out" | grep -q 'capacity_players=51' || { echo "ERROR: expected 51 players" >&2; exit 1; }
echo "$out" | grep -q 'occupancy_layout=50,1' || { echo "ERROR: expected occupancy_layout=50,1" >&2; exit 1; }
echo "$out" | grep -q 'map_capacity_ok=1' || { echo "ERROR: map_capacity_ok missing" >&2; exit 1; }

echo "test_map_capacity.sh PASS"
