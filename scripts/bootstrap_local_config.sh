#!/usr/bin/env bash
# 从 *.cnf.example 生成本地配置；不覆盖已有文件
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
CFG="$ROOT/config"
copied=0
skipped=0
for ex in "$CFG"/*.cnf.example; do
  [[ -f "$ex" ]] || continue
  base="$(basename "$ex" .example)"
  dest="$CFG/$base"
  if [[ -f "$dest" ]]; then
    echo "keep  $dest"
    skipped=$((skipped + 1))
  else
    cp -n "$ex" "$dest"
    echo "create $dest"
    copied=$((copied + 1))
  fi
done
echo "bootstrap_local_config: created=$copied kept=$skipped"
echo "提示: 按本机修改 config/mysql.cnf / redis.cnf 中的密码与地址"
