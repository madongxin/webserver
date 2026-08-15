#!/usr/bin/env bash
# 空库升级、重复 migrate、回滚、缺字段 verify 失败。使用独立数据库，不碰业务库表数据语义。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
CNF="${GAMEMESH_MYSQL_CNF:-$ROOT/config/mysql.cnf}"
[[ -f "$CNF" ]] || { echo "ERROR: missing $CNF" >&2; exit 1; }
command -v mysql >/dev/null 2>&1 || { echo "ERROR: mysql client missing" >&2; exit 1; }

DB="gamemesh_migtest_$$"
export GAMEMESH_MYSQL_DB="$DB"
cleanup() { mysql_admin "DROP DATABASE IF EXISTS \`$DB\`" || true; }
trap cleanup EXIT

mysql_admin() {
  python3 - "$CNF" "$1" <<'PY'
import os, subprocess, sys
cnf, sql = sys.argv[1], sys.argv[2]
kv={}
for line in open(cnf):
    line=line.strip()
    if not line or line.startswith("#") or "=" not in line: continue
    k,v=line.split("=",1); kv[k.strip()]=v.strip()
host=os.environ.get("GAMEMESH_MYSQL_HOST", kv.get("ip","127.0.0.1"))
port=os.environ.get("GAMEMESH_MYSQL_PORT", kv.get("port","3306"))
user=os.environ.get("GAMEMESH_MYSQL_USER", kv.get("username", kv.get("user","root")))
pw=os.environ.get("GAMEMESH_MYSQL_PASSWORD", kv.get("password",""))
cmd=["mysql","-h",host,"-P",str(port),"-u",user]
if pw:
    cmd.append("-p"+pw)
cmd += ["-e", sql]
raise SystemExit(subprocess.call(cmd))
PY
}

mysql_admin "CREATE DATABASE \`$DB\`"
"$ROOT/scripts/migrate_db.sh"
"$ROOT/scripts/migrate_db.sh"   # 重复执行
"$ROOT/scripts/migrate_db.sh" verify
"$ROOT/scripts/rollback_db.sh"
set +e
"$ROOT/scripts/migrate_db.sh" verify
rc=$?
set -e
[[ "$rc" -ne 0 ]] || { echo "ERROR: verify should fail after rollback" >&2; exit 1; }
"$ROOT/scripts/migrate_db.sh"
python3 - "$CNF" "$DB" <<'PY'
import os, subprocess, sys
cnf, db = sys.argv[1], sys.argv[2]
kv={}
for line in open(cnf):
    line=line.strip()
    if not line or line.startswith("#") or "=" not in line: continue
    k,v=line.split("=",1); kv[k.strip()]=v.strip()
host=os.environ.get("GAMEMESH_MYSQL_HOST", kv.get("ip","127.0.0.1"))
port=os.environ.get("GAMEMESH_MYSQL_PORT", kv.get("port","3306"))
user=os.environ.get("GAMEMESH_MYSQL_USER", kv.get("username", kv.get("user","root")))
pw=os.environ.get("GAMEMESH_MYSQL_PASSWORD", kv.get("password",""))
sql="ALTER TABLE player_profile DROP COLUMN move_speed"
cmd=["mysql","-h",host,"-P",str(port),"-u",user]
if pw:
    cmd.append("-p"+pw)
cmd += ["-D", db, "-e", sql]
raise SystemExit(subprocess.call(cmd))
PY
set +e
"$ROOT/scripts/migrate_db.sh" verify
rc=$?
set -e
[[ "$rc" -ne 0 ]] || { echo "ERROR: verify should fail when column missing" >&2; exit 1; }
echo "test_migrate_db.sh PASS"
