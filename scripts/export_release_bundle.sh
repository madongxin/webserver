#!/usr/bin/env bash
# 将 stable_gate 产物汇总到 run/release/<commit>/ 并写 manifest.json
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

COMMIT="${1:-$(git rev-parse HEAD 2>/dev/null || echo nogit)}"
DIRTY=0
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
    DIRTY=1
  fi
fi

OUT="$ROOT/run/release/${COMMIT}"
mkdir -p "$OUT/failover" "$OUT/sanitizers"
VERDICT="${STABLE_VERDICT:-STABLE BLOCKED}"
EXIT_CODE="${STABLE_EXIT_CODE:-1}"
STEPS_JSON="${STABLE_STEPS_JSON:-[]}"

MISSING_FILE="$(mktemp)"
: >"$MISSING_FILE"
copy_if() {
  local src="$1" dst="$2" label="$3"
  if [[ -f "$src" ]]; then
    cp -f "$src" "$dst"
  else
    echo "$label" >>"$MISSING_FILE"
  fi
}

latest_glob() {
  # $1=dir $2=pattern → newest path or empty
  local dir="$1" pat="$2" newest="" f
  shopt -s nullglob
  for f in "$dir"/$pat; do
    if [[ -z "$newest" || "$f" -nt "$newest" ]]; then
      newest="$f"
    fi
  done
  shopt -u nullglob
  echo "$newest"
}

LATEST_LOAD_JSON="$(latest_glob "$ROOT/run/load" 'load_*.json')"
LATEST_LOAD_TXT="$(latest_glob "$ROOT/run/load" 'load_*.txt')"
LATEST_SOAK_JSON="$(latest_glob "$ROOT/run/soak" 'soak_*.json')"
LATEST_SOAK_TXT="$(latest_glob "$ROOT/run/soak" 'soak_*.txt')"
SUMMARY="$(latest_glob "$ROOT/run/stable_gate" 'summary_*.json')"

if [[ -n "$LATEST_LOAD_JSON" ]]; then
  copy_if "$LATEST_LOAD_JSON" "$OUT/load-report.json" load-report
elif [[ -n "$LATEST_LOAD_TXT" ]]; then
  copy_if "$LATEST_LOAD_TXT" "$OUT/load-report.txt" load-report
else
  echo load-report >>"$MISSING_FILE"
fi
if [[ -n "$LATEST_SOAK_JSON" ]]; then
  copy_if "$LATEST_SOAK_JSON" "$OUT/soak-report.json" soak-report
elif [[ -n "$LATEST_SOAK_TXT" ]]; then
  copy_if "$LATEST_SOAK_TXT" "$OUT/soak-report.txt" soak-report
else
  echo soak-report >>"$MISSING_FILE"
fi
copy_if "${SUMMARY:-}" "$OUT/stable_gate_summary.json" stable_gate_summary

for name in build-debug.log build-release.log unit.log integration.log e2e-20x.log; do
  if [[ -f "$ROOT/run/stable_gate/$name" ]]; then
    cp -f "$ROOT/run/stable_gate/$name" "$OUT/$name"
  fi
done
shopt -s nullglob
for f in "$ROOT"/run/stable_gate/failover/*.log; do cp -f "$f" "$OUT/failover/"; done
for f in "$ROOT"/run/stable_gate/sanitizers/*.log; do cp -f "$f" "$OUT/sanitizers/"; done
shopt -u nullglob

export OUT COMMIT DIRTY VERDICT EXIT_CODE STEPS_JSON MISSING_FILE ROOT
python3 <<'PY'
import hashlib, json, os, subprocess, datetime

def sh(cmd):
    try:
        return subprocess.check_output(cmd, shell=True, text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ""

out = os.environ["OUT"]
missing = []
with open(os.environ["MISSING_FILE"], encoding="utf-8") as f:
    missing = [ln.strip() for ln in f if ln.strip()]

hashes = {}
for dirpath, _, files in os.walk(out):
    for fn in files:
        if fn == "manifest.json":
            continue
        path = os.path.join(dirpath, fn)
        rel = os.path.relpath(path, out)
        h = hashlib.sha256()
        with open(path, "rb") as bf:
            for chunk in iter(lambda: bf.read(1 << 20), b""):
                h.update(chunk)
        hashes[rel] = h.hexdigest()

try:
    steps = json.loads(os.environ.get("STEPS_JSON") or "[]")
except Exception:
    steps = []

brpc_header = ""
for p in [
    "/usr/local/include/brpc/server.h",
    "/usr/include/brpc/server.h",
    os.path.expanduser("~/.local/gamemesh-deps/include/brpc/server.h"),
]:
    if os.path.isfile(p):
        brpc_header = p
        break

manifest = {
    "commit": os.environ["COMMIT"],
    "dirty": os.environ["DIRTY"] == "1",
    "verdict": os.environ["VERDICT"],
    "exit_code": int(os.environ["EXIT_CODE"]),
    "generated_at": datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
    "toolchain": {
        "compiler": sh("g++ --version | head -1"),
        "cmake": sh("cmake --version | head -1"),
        "protoc": sh("protoc --version"),
        "brpc_header": brpc_header,
        "brpc_version_pin": os.environ.get("GAMEMESH_BRPC_VERSION", "1.9.0"),
        "deps_prefix": os.environ.get("GAMEMESH_DEPS_PREFIX", ""),
        "uname": sh("uname -a"),
        "nproc": sh("nproc"),
        "mem_kb": sh("awk '/MemTotal/{print $2}' /proc/meminfo"),
    },
    "steps": steps,
    "missing_artifacts": missing,
    "report_sha256": hashes,
    "report_dir": f"run/release/{os.environ['COMMIT']}",
}
path = os.path.join(out, "manifest.json")
with open(path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False)
    f.write("\n")
print(path)
print(manifest["verdict"])
PY
rm -f "$MISSING_FILE"
echo "export_release_bundle.sh PASS dir=$OUT"
