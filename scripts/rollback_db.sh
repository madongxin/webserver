#!/usr/bin/env bash
# 回滚最近一条 config/migrations/*_down.sql，并删除 schema_migrations 记录。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
CNF="${GAMEMESH_MYSQL_CNF:-$ROOT/config/mysql.cnf}"
[[ -f "$CNF" ]] || { echo "ERROR: missing $CNF" >&2; exit 1; }

python3 - "$ROOT" "$CNF" <<'PY'
import glob, os, re, subprocess, sys

root, cnf = sys.argv[1], sys.argv[2]
kv = {}
for line in open(cnf):
    line = line.strip()
    if not line or line.startswith("#") or "=" not in line:
        continue
    k, v = line.split("=", 1)
    kv[k.strip()] = v.strip()
host = os.environ.get("GAMEMESH_MYSQL_HOST", kv.get("ip", "127.0.0.1"))
port = os.environ.get("GAMEMESH_MYSQL_PORT", kv.get("port", "3306"))
user = os.environ.get("GAMEMESH_MYSQL_USER", kv.get("username", kv.get("user", "root")))
pw = os.environ.get("GAMEMESH_MYSQL_PASSWORD", kv.get("password", ""))
db = os.environ.get("GAMEMESH_MYSQL_DB", kv.get("dbname", "metrics"))

def mysql(sql):
    cmd = ["mysql", "-h", host, "-P", str(port), "-u", user, "--batch", "--raw", "-N", "-D", db]
    if pw:
        cmd.append("-p" + pw)
    cmd += ["-e", sql]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if p.returncode != 0:
        sys.stderr.write(p.stderr or "mysql failed\n")
        sys.exit(1)
    return p.stdout

row = mysql("SELECT version, name FROM schema_migrations ORDER BY version DESC LIMIT 1").strip()
if not row:
    sys.stderr.write("ERROR: no applied migrations\n")
    sys.exit(1)
ver_s, name = row.split("\t", 1)
ver = int(ver_s)
down = os.path.join(root, "config/migrations", "%04d_%s_down.sql" % (ver, name))
if not os.path.isfile(down):
    sys.stderr.write("ERROR: missing %s\n" % down)
    sys.exit(1)
mysql(open(down).read())
mysql("DELETE FROM schema_migrations WHERE version=%d" % ver)
print("rolled back %04d_%s" % (ver, name))
PY
