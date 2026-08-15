#!/usr/bin/env bash
# 阶段 0+ 单元测试（失败立即退出，禁止吞错）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BIN="${ROOT}/build/test"

run_one() {
  local name="$1"
  shift
  if [[ ! -x "$BIN/$name" ]]; then
    echo "ERROR missing binary: $BIN/$name (build first)" >&2
    exit 1
  fi
  echo "== $name =="
  "$BIN/$name" "$@"
}

run_one reactor_unit_test
run_one message_route_test
run_one map_placement_test
run_one map_static_data_test
run_one map_runtime_aoi_test
run_one player_serial_queue_test
run_one player_serial_shutdown_test
run_one secure_random_test
run_one asset_dirty_consistency_test
run_one auth_session_boundary_test
run_one phase1_gateway_boundary_test
run_one phase1_correctness_test
run_one trusted_player_id_test
run_one placement_authority_test
run_one placement_formal_fence_test
run_one command_policy_test
run_one client_protocol_test
run_one utf8_text_test
run_one gateway_overload_test
run_one phase2_transfer_snapshot_test
run_one player_serial_async_test
run_one phase1_channel_hold_test
run_one channel_snapshot_race_test
run_one gateway_disconnect_async_test
run_one gateway_conn_race_test
run_one discovery_ha_test
run_one push_replay_cache_test
run_one password_hash_test
run_one service_health_test
run_one formal_mysql_boundary_test
# 协议导出：未改 proto 时 game.proto SHA-256 稳定
proto_a="$(mktemp -d)"
proto_b="$(mktemp -d)"
"$ROOT/scripts/export_unity_protocol.sh" "$proto_a"
"$ROOT/scripts/export_unity_protocol.sh" "$proto_b"
if ! diff -q "$proto_a/game.proto.sha256" "$proto_b/game.proto.sha256" >/dev/null; then
  echo "ERROR: export_unity_protocol.sh proto hash not stable" >&2
  exit 1
fi
rm -rf "$proto_a" "$proto_b"
# phase3_discovery_test / phase3_gamedb_asset_test：依赖 Redis/MySQL，见 test_integration.sh
# session_store_test / placement_store_test 依赖 Redis：scripts/test_placement.sh / test_integration.sh
echo "test_unit.sh PASS"
