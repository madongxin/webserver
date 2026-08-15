/**
 * S2：AOI 同格/跨格/进出视野/跨图隔离；Move 拒绝 NaN/越界/不可走/超速/旧 seq。
 */
#include "MapRuntime.h"
#include "MapStaticData.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

std::shared_ptr<const MapStaticData> MakeOpen() {
    const std::string json = R"JSON({
      "schema_version": 1,
      "map_template_id": 9101,
      "scene_name": "Open",
      "data_version": 1,
      "bounds_min": [0, 0, 0],
      "bounds_max": [40, 5, 40],
      "aoi_cell_size": 10.0,
      "nav_sample_step": 1.0,
      "grid_width": 40,
      "grid_height": 40,
      "walkable_rle": [1, 1600],
      "spawn_points": [{"id":"default","position":[1, 0, 1],"yaw":0}]
    })JSON";
    std::shared_ptr<const MapStaticData> d;
    std::string err;
    if (!MapStaticData::LoadFromJson(json, &d, &err)) {
        std::printf("FAIL make open: %s\n", err.c_str());
        return nullptr;
    }
    return d;
}

MapEntity Ent(uint64_t id, float x, float z, const char *gw = "gw-0") {
    MapEntity e;
    e.player_id = id;
    e.player_name = "p" + std::to_string(id);
    e.x = x;
    e.y = 0;
    e.z = z;
    e.hp = 100;
    e.max_hp = 100;
    e.move_speed = 10.f;
    e.gateway_instance_id = gw;
    e.session_id = "s" + std::to_string(id);
    return e;
}

int CountOp(const AoiPushBatch &b, uint64_t recipient, int op) {
    int n = 0;
    for (const auto &e : b.events) {
        if (e.recipient_id == recipient && e.op == op)
            ++n;
    }
    return n;
}

}  // namespace

int main() {
    auto data = MakeOpen();
    if (!data)
        return 1;
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetViewRadiusCells(1);  // 3x3 AOI cells of size 10 → view ~30m

    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(1, data, Ent(1, 1, 1), &self, &snap, &pushes, &err), "enter p1");
    Expect(snap.empty(), "p1 alone no snapshot");

    pushes = {};
    Expect(rt.Enter(1, data, Ent(2, 2, 2), &self, &snap, &pushes, &err), "enter p2 same cell");
    Expect(snap.size() == 1 && snap[0].player_id == 1, "p2 sees p1");
    Expect(CountOp(pushes, 1, 1) == 1, "p1 got ENTER p2");

    // 跨格但仍在视野（cell 0 与 cell 1，radius=1）
    pushes = {};
    MapEntity conf;
    Expect(rt.Move(1, 2, 12, 0, 2, 0, 1, 1000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "p2 move nearby");
    Expect(CountOp(pushes, 1, 2) == 1, "p1 got MOVE");

    // 走出视野（cell 3：x=35）
    pushes = {};
    Expect(rt.Move(1, 2, 35, 0, 2, 0, 2, 4000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "p2 leave view");
    Expect(CountOp(pushes, 1, 3) == 1, "p1 got LEAVE");

    // 跨地图隔离
    pushes = {};
    Expect(rt.Enter(2, data, Ent(3, 1, 1), &self, &snap, &pushes, &err), "enter p3 other map");
    Expect(snap.empty(), "p3 cannot see map1");
    Expect(CountOp(pushes, 1, 1) == 0 && CountOp(pushes, 2, 1) == 0, "no cross-map ENTER");

    // 断线隐藏
    pushes = {};
    Expect(rt.Disconnect(1, &pushes), "disconnect p1");
    Expect(CountOp(pushes, 2, 3) == 0, "p2 already out of view");  // p2 far
    // p3 on other map
    Expect(rt.HasPlayer(1, 1), "slot kept after disconnect");

    // 拒绝项（p2 仍在图 1）
    Expect(rt.Move(1, 2, std::numeric_limits<float>::quiet_NaN(), 0, 2, 0, 3, 5000, &conf, &pushes,
                   &err) == MapMoveReject::NotFinite,
           "nan rejected");
    Expect(rt.Move(1, 2, 99, 0, 2, 0, 3, 5000, &conf, &pushes, &err) == MapMoveReject::OutOfBounds,
           "oob rejected");
    // 不可走：另造带洞图
    const std::string hole = R"JSON({
      "schema_version": 1, "map_template_id": 9102, "scene_name": "Hole", "data_version": 1,
      "bounds_min": [0,0,0], "bounds_max": [10,2,10], "aoi_cell_size": 5, "nav_sample_step": 1,
      "grid_width": 10, "grid_height": 10, "walkable_rle": [1, 50, 0, 10, 1, 40],
      "spawn_points": [{"id":"default","position":[1,0,1],"yaw":0}]
    })JSON";
    std::shared_ptr<const MapStaticData> hd;
    Expect(MapStaticData::LoadFromJson(hole, &hd, &err), "hole map");
    rt.ClearForTest();
    Expect(rt.Enter(3, hd, Ent(8, 1, 1), &self, &snap, &pushes, &err), "enter hole");
    // row 5 (z=5.5) first 10 cells of that row are the 0-run starting at index 50
    Expect(rt.Move(3, 8, 0.5f, 0, 5.5f, 0, 1, 2000, &conf, &pushes, &err) ==
               MapMoveReject::Unwalkable,
           "unwalkable rejected");
    Expect(rt.Move(3, 8, 2, 0, 1, 0, 1, 2000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "small step ok");
    Expect(rt.Move(3, 8, 9, 0, 1, 0, 1, 2001, &conf, &pushes, &err) == MapMoveReject::StaleSeq,
           "stale seq rejected");
    Expect(rt.Move(3, 8, 9, 0, 1, 0, 2, 2100, &conf, &pushes, &err) == MapMoveReject::TooFast,
           "too fast rejected");

    if (fails) {
        std::printf("map_runtime_aoi_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK map_runtime_aoi_test\n");
    return 0;
}
