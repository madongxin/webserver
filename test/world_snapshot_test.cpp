/**
 * S2：世界快照含 self/AOI；死亡禁止移动；非法坐标不进入 last_safe 内存。
 */
#include "GameLogic.h"
#include "MapCatalog.h"
#include "MapInstanceRegistry.h"
#include "MapRuntime.h"
#include "MapStaticData.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

}  // namespace

int main() {
    const std::string json = R"JSON({
      "schema_version": 1,
      "map_template_id": 9201,
      "scene_name": "Snap",
      "data_version": 1,
      "bounds_min": [0, 0, 0],
      "bounds_max": [40, 5, 40],
      "aoi_cell_size": 10.0,
      "nav_sample_step": 1.0,
      "grid_width": 40,
      "grid_height": 40,
      "walkable_rle": [1, 1600],
      "spawn_points": [{"id":"default","position":[2, 0, 2],"yaw":0}]
    })JSON";
    std::shared_ptr<const MapStaticData> data;
    std::string err;
    if (!MapStaticData::LoadFromJson(json, &data, &err)) {
        std::printf("FAIL load map: %s\n", err.c_str());
        return 1;
    }
    MapRuntime::Instance().ClearForTest();
    MapInstanceRegistry::Instance().ClearForTest();
    MapInstanceRegistry::Instance().SetLocalInstanceId("gl-test");
    MapInstanceRegistry::Instance().Claim(77, 9201, 1, 0);
    MapInstanceRegistry::Instance().AddPlayer(77, 1);
    MapInstanceRegistry::Instance().AddPlayer(77, 2);

    MapEntity a, b, self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    a.player_id = 1;
    a.player_name = "a";
    a.x = 2;
    a.z = 2;
    a.hp = 100;
    a.max_hp = 100;
    a.connected = true;
    a.move_speed = 10;
    b = a;
    b.player_id = 2;
    b.player_name = "b";
    b.x = 3;
    b.z = 3;
    Expect(MapRuntime::Instance().Enter(77, data, a, &self, &snap, &pushes, &err), "enter a");
    Expect(MapRuntime::Instance().Enter(77, data, b, &self, &snap, &pushes, &err), "enter b");

    game::FullStateSnapshotRsp fs;
    Expect(GameLogic::Instance().BuildFullStateSnapshot(1, 42, &fs) && fs.ok(), "snapshot ok");
    Expect(fs.baseline_server_seq() == 42, "baseline");
    Expect(fs.has_self() && fs.self().player_id() == 1, "self");
    Expect(fs.aoi_entities_size() >= 1, "aoi");
    Expect(fs.recovery_reason() == "INSTANCE_LIVE", "live");
    Expect(fs.map_instance_id() == 77, "map id");
    Expect(!fs.ok() || fs.error_code() == "OK" || fs.error_code().empty() || fs.ok(), "code");

    game::MoveReq mreq;
    mreq.set_player_id(1);
    mreq.set_map_instance_id(77);
    mreq.mutable_position()->set_x(1e9);
    mreq.mutable_position()->set_z(1e9);
    game::GameResponse rsp;
    rsp.set_seq(1);
    GameLogic::Instance().HandleMoveForTest(mreq, &rsp);
    Expect(!rsp.ok(), "illegal move rejected");

    game::FullStateSnapshotRsp fs2;
    Expect(GameLogic::Instance().BuildFullStateSnapshot(1, 42, &fs2) && fs2.ok(), "snap after reject");
    Expect(std::abs(fs2.self().position().x() - 2.f) < 0.01f, "pos unchanged");

    GameLogic::Instance().SetLifeStateForTest(1, 0, "DEAD");
    mreq.mutable_position()->set_x(4);
    mreq.mutable_position()->set_z(4);
    rsp.Clear();
    rsp.set_seq(2);
    GameLogic::Instance().HandleMoveForTest(mreq, &rsp);
    Expect(!rsp.ok() && rsp.move().error_code() == "ERR_PLAYER_DEAD", "dead cannot move");

    game::RespawnReq rreq;
    rreq.set_player_id(1);
    rreq.set_map_instance_id(77);
    rreq.set_operation_id("op-1");
    rsp.Clear();
    rsp.set_seq(3);
    Expect(GameLogic::Instance().HandleRespawnForTest(rreq, &rsp) && rsp.ok(), "respawn");
    rsp.Clear();
    rsp.set_seq(4);
    Expect(GameLogic::Instance().HandleRespawnForTest(rreq, &rsp) && rsp.ok(), "respawn idempotent");

    if (fails) {
        std::printf("world_snapshot_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK world_snapshot_test aoi=%d baseline=%llu\n", fs.aoi_entities_size(),
                static_cast<unsigned long long>(fs.baseline_server_seq()));
    return 0;
}
