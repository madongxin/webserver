#!/usr/bin/env bash
# 从运行中的 server 拉取 /metrics 落盘到 docs/mmo-migration/baselines/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_URL="${1:-http://127.0.0.1:8080}"
OUT_DIR="$ROOT/docs/mmo-migration/baselines"
mkdir -p "$OUT_DIR"

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$OUT_DIR/${STAMP}-metrics.txt"
META="$OUT_DIR/${STAMP}-meta.txt"

{
  echo "time=$(date -Iseconds)"
  echo "base_url=$BASE_URL"
  if git -C "$ROOT" rev-parse --short HEAD >/dev/null 2>&1; then
    echo "git=$(git -C "$ROOT" rev-parse --short HEAD)"
    echo "git_dirty=$(git -C "$ROOT" status --porcelain | wc -l)"
  else
    echo "git=unknown"
  fi
  echo "host=$(hostname)"
  echo "uname=$(uname -a)"
} >"$META"

if ! curl -fsS -m 5 "$BASE_URL/metrics" -o "$OUT"; then
  echo "FAILED: cannot fetch $BASE_URL/metrics" >&2
  echo "Start server first: $ROOT/build/test/server 8080 8081" >&2
  rm -f "$OUT"
  exit 1
fi

echo "Wrote $OUT"
echo "Wrote $META"
echo "---- preview ----"
head -n 40 "$OUT"
