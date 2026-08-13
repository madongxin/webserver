#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# 集成项：依赖 Redis/MySQL 的必需测试；Redis/MySQL 不可用必须失败（禁止 SKIP→成功）
wait_redis() {
  local pass="" i
  [[ -f "$ROOT/config/redis.cnf" ]] && pass="$(grep -E '^password=' "$ROOT/config/redis.cnf" | head -1 | cut -d= -f2- || true)"
  command -v redis-cli >/dev/null 2>&1 || { echo "ERROR: redis-cli missing"; exit 1; }
  for i in $(seq 1 30); do
    if [[ -n "$pass" ]]; then
      redis-cli -a "$pass" --no-auth-warning ping 2>/dev/null | grep -q PONG && return 0
    else
      redis-cli ping 2>/dev/null | grep -q PONG && return 0
    fi
    sleep 1
  done
  echo "ERROR: Redis not ready after wait"
  exit 1
}
wait_mysql() {
  local i
  [[ -f "$ROOT/config/mysql.cnf" ]] || { echo "ERROR: config/mysql.cnf missing"; exit 1; }
  command -v mysql >/dev/null 2>&1 || { echo "ERROR: mysql client missing"; exit 1; }
  for i in $(seq 1 30); do
    if python3 - <<'PY'
import subprocess
kv={}
for line in open("config/mysql.cnf"):
    line=line.strip()
    if not line or line.startswith("#") or "=" not in line: continue
    k,v=line.split("=",1); kv[k.strip()]=v.strip()
host=kv.get("ip","127.0.0.1"); port=kv.get("port","3306")
user=kv.get("username", kv.get("user","root")); pw=kv.get("password","")
db=kv.get("dbname","metrics")
cmd=["mysql","-h",host,"-P",str(port),"-u",user,db,"-e","SELECT 1"]
if pw: cmd.insert(-2, f"-p{pw}")
raise SystemExit(0 if subprocess.call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)==0 else 1)
PY
    then
      return 0
    fi
    sleep 1
  done
  echo "ERROR: MySQL not ready after wait"
  exit 1
}
wait_redis
wait_mysql
./scripts/test_placement.sh
./build/test/session_store_test
./build/test/player_transfer_test
./build/test/placement_recovery_test
./build/test/phase3_discovery_test
./scripts/test_push_reconnect.sh
./build/test/push_ack_redis_test
./build/test/push_full_snapshot_test

# 阶段一必需 MySQL 门禁：不可用必须失败
if [[ ! -x ./build/test/gamedb_snapshot_idempotency_test ]]; then
  echo "ERROR: missing gamedb_snapshot_idempotency_test"
  exit 1
fi
./build/test/gamedb_snapshot_idempotency_test
./build/test/gamedb_mutation_idempotency_test
./build/test/gamedb_mutation_atomicity_test
./build/test/gamedb_unknown_result_test
if [[ -x ./build/test/phase3_gamedb_asset_test ]]; then
  ./build/test/phase3_gamedb_asset_test
fi
if [[ ! -x ./build/test/gamedb_mail_claim_test ]]; then
  echo "ERROR: missing gamedb_mail_claim_test"
  exit 1
fi
./build/test/gamedb_mail_claim_test
if [[ ! -x ./build/test/healthy_logic_refresh_test ]]; then
  echo "ERROR: missing healthy_logic_refresh_test"
  exit 1
fi
./build/test/healthy_logic_refresh_test
echo "test_integration.sh PASS"
