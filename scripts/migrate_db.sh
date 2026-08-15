#!/usr/bin/env bash
# 应用 config/migrations/*_up.sql。可重复；checksum 变化则失败（不吞错）。
# 用法：./scripts/migrate_db.sh [verify]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
CNF="${GAMEMESH_MYSQL_CNF:-$ROOT/config/mysql.cnf}"
[[ -f "$CNF" ]] || { echo "ERROR: missing $CNF" >&2; exit 1; }
command -v mysql >/dev/null 2>&1 || { echo "ERROR: mysql client missing" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 missing" >&2; exit 1; }

MODE="${1:-apply}"

python3 - "$ROOT" "$CNF" "$MODE" <<'PY'
import glob, hashlib, os, re, subprocess, sys

root, cnf, mode = sys.argv[1], sys.argv[2], sys.argv[3]
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

def mysql(sql, dbname=db):
    cmd = ["mysql", "-h", host, "-P", str(port), "-u", user, "--batch", "--raw", "-N"]
    if pw:
        cmd.append("-p" + pw)
    if dbname:
        cmd += ["-D", dbname]
    cmd += ["-e", sql]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if p.returncode != 0:
        sys.stderr.write(p.stderr or p.stdout or "mysql failed\n")
        sys.exit(1)
    return p.stdout

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()

mysql("CREATE DATABASE IF NOT EXISTS `%s`" % db.replace("`", ""), dbname=None)
mysql("""CREATE TABLE IF NOT EXISTS schema_migrations (
  version INT NOT NULL PRIMARY KEY,
  name VARCHAR(128) NOT NULL,
  checksum CHAR(64) NOT NULL,
  applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4""")

files = sorted(glob.glob(os.path.join(root, "config/migrations", "*_up.sql")))
applied = {}
raw = mysql("SELECT version, name, checksum FROM schema_migrations")
for line in raw.splitlines():
    if not line.strip():
        continue
    parts = line.split("\t")
    applied[int(parts[0])] = (parts[1], parts[2])

required_profile_cols = [
    "player_id", "player_name", "hp", "max_hp", "mp", "max_mp", "attack", "spell_power",
    "defense", "magic_resistance", "crit_chance", "crit_damage", "move_speed",
    "attack_speed", "stats_version",
]
required_safe_cols = [
    "player_id", "realm_id", "map_template_id", "last_safe_x", "last_safe_y", "last_safe_z",
    "last_safe_yaw", "position_version",
]

def verify_profile():
    tables = mysql("SHOW TABLES LIKE 'player_profile'").strip()
    if not tables:
        sys.stderr.write("ERROR: player_profile missing; run ./scripts/migrate_db.sh\n")
        sys.exit(1)
    cols = set()
    for line in mysql("SHOW COLUMNS FROM player_profile").splitlines():
        if line.strip():
            cols.add(line.split("\t")[0])
    missing = [c for c in required_profile_cols if c not in cols]
    if missing:
        sys.stderr.write("ERROR: player_profile missing columns: %s\n" % ",".join(missing))
        sys.exit(1)
    print("verify player_profile ok")

def verify_last_safe():
    tables = mysql("SHOW TABLES LIKE 'player_last_safe_position'").strip()
    if not tables:
        sys.stderr.write("ERROR: player_last_safe_position missing; run ./scripts/migrate_db.sh\n")
        sys.exit(1)
    cols = set()
    for line in mysql("SHOW COLUMNS FROM player_last_safe_position").splitlines():
        if line.strip():
            cols.add(line.split("\t")[0])
    missing = [c for c in required_safe_cols if c not in cols]
    if missing:
        sys.stderr.write("ERROR: player_last_safe_position missing columns: %s\n" % ",".join(missing))
        sys.exit(1)
    print("verify player_last_safe_position ok")

if mode == "verify":
    verify_profile()
    verify_last_safe()
    sys.exit(0)

for path in files:
    base = os.path.basename(path)
    m = re.match(r"^(\d+)_([a-z0-9_]+)_up\.sql$", base)
    if not m:
        sys.stderr.write("ERROR: bad migration name %s\n" % base)
        sys.exit(1)
    ver = int(m.group(1))
    name = m.group(2)
    checksum = sha256_file(path)
    if ver in applied:
        old_name, old_sum = applied[ver]
        if old_sum != checksum:
            sys.stderr.write("ERROR: migration %d checksum mismatch applied=%s file=%s\n" %
                             (ver, old_sum, checksum))
            sys.exit(1)
        print("skip %s (already applied)" % base)
        continue
    sql = open(path).read()
    mysql(sql)
    mysql("INSERT INTO schema_migrations(version,name,checksum) VALUES(%d,'%s','%s')" %
          (ver, name.replace("'", ""), checksum))
    print("applied %s checksum=%s" % (base, checksum))

verify_profile()
verify_last_safe()
print("migrate_db.sh PASS")
PY
