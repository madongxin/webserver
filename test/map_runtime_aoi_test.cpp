/**
 * S2：AOI 同格/跨格/进出视野/跨图隔离；Move 拒绝 NaN/越界/不可走/旧 seq；超速钳位接受。
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
    rt.SetMaxVisible(50);      // 这条用例钉住旧的 50 人裁剪；进程默认是 200

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
    // row 5 (z=5.5) 整行不可走：沿方向收到洞边，接受并回权威坐标，不整包拒绝。
    Expect(rt.Move(3, 8, 0.5f, 0, 5.5f, 0, 1, 2000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "unwalkable slid ok");
    Expect(hd->IsWalkable(conf.x, conf.z), "slid point walkable");
    Expect(conf.z > 1.f && conf.z < 5.f, "stopped before hole row");
    rt.ClearForTest();
    Expect(rt.Enter(3, hd, Ent(8, 1, 1), &self, &snap, &pushes, &err), "reenter hole");
    Expect(rt.Move(3, 8, 2, 0, 1, 0, 1, 2000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "small step ok");
    Expect(rt.Move(3, 8, 9, 0, 1, 0, 1, 2001, &conf, &pushes, &err) == MapMoveReject::StaleSeq,
           "stale seq rejected");
    // 超速：沿方向钳到本帧合法距离后接受，避免大跳拉回。
    Expect(rt.Move(3, 8, 9, 0, 1, 0, 2, 2100, &conf, &pushes, &err) == MapMoveReject::Ok,
           "too fast clamped ok");
    Expect(conf.x > 2.f && conf.x < 9.f, "clamped x between start and target");
    Expect(std::fabs(conf.z - 1.f) < 1e-3f, "clamped z unchanged");

    // 每实例 AOI 半径 + Unload
    rt.ClearForTest();
    rt.SetViewRadiusCells(8);
    pushes = {};
    Expect(rt.Enter(11, data, Ent(21, 1, 1), &self, &snap, &pushes, &err, 0),
           "enter tight aoi");
    Expect(rt.Enter(11, data, Ent(22, 15, 1), &self, &snap, &pushes, &err, 0),
           "enter p22 same tight instance");
    Expect(snap.empty(), "radius 0 cannot see other cell");
    Expect(rt.Unload(11), "unload instance");
    Expect(!rt.HasPlayer(11, 21) && !rt.HasPlayer(11, 22), "unloaded players gone");

    // 同一格超过 50 人时，只同步最近的 50 个（距离相同按 player_id）。
    rt.ClearForTest();
    rt.SetViewRadiusCells(1);
    pushes = {};
    for (uint64_t id = 1; id <= 60; ++id) {
        pushes = {};
        Expect(rt.Enter(9, data, Ent(id, 1, 1), &self, &snap, &pushes, &err), "enter crowd");
    }
    Expect(snap.size() == 50, "enter snapshot capped at 50");
    int enter_n = 0;
    for (const auto &ev : pushes.events) {
        if (ev.op == 1 && ev.snapshot.player_id == 60)
            ++enter_n;
    }
    Expect(enter_n == 0, "player 60 is outside every viewer's nearest 50");
    std::vector<MapEntity> snap1;
    Expect(rt.SnapshotView(1, nullptr, &snap1, nullptr), "snapshot p1");
    bool sees60 = false;
    for (const auto &e : snap1) {
        if (e.player_id == 60)
            sees60 = true;
    }
    Expect(!sees60 && snap1.size() == 50, "p1 subscribes to 50 nearer players");

    if (fails) {
        std::printf("map_runtime_aoi_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK map_runtime_aoi_test\n");
    return 0;
}
