#!/usr/bin/env bash
# Linux/Remote SSH：重新生成 compile_commands.json
set -euo pipefail
cd "$(dirname "$0")/.."

echo ">>> cmake configure (build/)"
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo ">>> sync compile_commands.json to project root"
cmake --build build --target ide_compile_commands

if [[ -f compile_commands.json ]]; then
  echo "OK: compile_commands.json ready ($(wc -l < compile_commands.json) lines)"
  echo "Next: Cursor -> Ctrl+Shift+P -> Developer: Reload Window"
else
  echo "ERROR: compile_commands.json not found" >&2
  exit 1
fi
