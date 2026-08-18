#!/usr/bin/env bash
# 从 inventory.tsv 生成 Prometheus file_sd（job=gamemesh，按 role/instance_id 打标）。
# 用法:
#   GAMEMESH_RUN_DIR=run/formal ./scripts/sync_prometheus_sd.sh
#   ./scripts/sync_prometheus_sd.sh /etc/prometheus/file_sd/gamemesh.json
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INV="${GAMEMESH_RUN_DIR:-$ROOT/run/formal}/inventory.tsv"
OUT_DEFAULT="/etc/prometheus/file_sd/gamemesh.json"
OUT="${1:-}"
if [[ -z "$OUT" ]]; then
  if [[ -d /etc/prometheus ]]; then
    OUT="$OUT_DEFAULT"
  else
    OUT="${GAMEMESH_RUN_DIR:-$ROOT/run/formal}/prometheus_file_sd.json"
  fi
fi

python3 - "$INV" "$OUT" <<'PY'
import json, os, sys

inv, out = sys.argv[1], sys.argv[2]
entries = []
if os.path.isfile(inv):
    with open(inv) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 5:
                continue
            role, iid, _pid, _rpc, http = parts[0], parts[1], parts[2], parts[3], parts[4]
            if http in ("-", "", "0"):
                continue
            port = http.rsplit(":", 1)[-1]
            try:
                p = int(port)
            except ValueError:
                continue
            if p <= 0:
                continue
            entries.append({
                "targets": ["127.0.0.1:%d" % p],
                "labels": {
                    "role": role,
                    "instance_id": iid,
                },
            })

os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
tmp = out + ".tmp"
with open(tmp, "w") as f:
    json.dump(entries, f, indent=2)
    f.write("\n")
os.replace(tmp, out)
print("wrote %s (%d targets)" % (out, len(entries)))
PY
