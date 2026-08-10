#!/usr/bin/env bash
# 阶段二：Channel 快照并发专项
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

[[ -x "$ROOT/build/test/channel_snapshot_race_test" ]] || {
  echo "ERROR: missing channel_snapshot_race_test (./scripts/build.sh Debug)"; exit 1
}
"$ROOT/build/test/channel_snapshot_race_test"
[[ -x "$ROOT/build/test/phase1_channel_hold_test" ]] && "$ROOT/build/test/phase1_channel_hold_test"
[[ -x "$ROOT/build/test/player_serial_async_test" ]] && "$ROOT/build/test/player_serial_async_test"
echo "test_channel_snapshot_race.sh PASS"
