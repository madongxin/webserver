#!/usr/bin/env bash
# 阶段二：Registry 临时不可用时保留最后有效 Channel（空 Discover 不清空）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

[[ -x "$ROOT/build/test/channel_snapshot_race_test" ]] || {
  echo "ERROR: build channel_snapshot_race_test"; exit 1
}
[[ -x "$ROOT/build/test/phase1_channel_hold_test" ]] && "$ROOT/build/test/phase1_channel_hold_test"
"$ROOT/build/test/channel_snapshot_race_test"

# discovery 空列表策略
[[ -x "$ROOT/build/test/discovery_ha_test" ]] && "$ROOT/build/test/discovery_ha_test"

echo "test_registry_outage.sh PASS"
