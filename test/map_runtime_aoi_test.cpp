/**
 * S2：AOI 同格/跨格/进出视野/跨图隔离；Move 拒绝 NaN/越界/不可走/旧 seq；超速钳位接受。
 */
#include "AoiPublishQueue.h"
#include "MapRuntime.h"
#include "MapStaticData.h"
#include "ServerStats.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

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
    const auto lock_samples_before = ServerStats::aoi_lock_samples.load();
    const auto lock_hold_before = ServerStats::aoi_lock_hold_us_sum.load();
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
    // 原地把 Y 掉出盒子：接受水平位置，高度留在站立处，不落到盒底。
    Expect(rt.Move(1, 2, 12, -10, 2, 0, 4, 6000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "fall through kept standing");
    Expect(conf.y > -0.5f && conf.y < 0.5f, "fall y stays at stand");
    // 不可走：另造带洞图。速度以内的请求按客户端坐标接受，不能滑回格子边把人钉住。
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
    Expect(rt.Move(3, 8, 0.5f, 0, 5.5f, 0, 1, 2000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "within speed crosses unwalkable");
    Expect(std::fabs(conf.x - 0.5f) < 0.05f && std::fabs(conf.z - 5.5f) < 0.05f,
           "accepted requested point");
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

    // 全量视野不读 visible_targets。人还在这张图上、在线、32 米内，即使站着不动也要进列表。
    rt.ClearForTest();
    rt.SetViewRadiusCells(1);
    pushes = {};
    Expect(rt.Enter(21, data, Ent(819, 1, 1), &self, &snap, &pushes, &err), "enter 819");
    Expect(rt.Enter(21, data, Ent(820, 8, 1), &self, &snap, &pushes, &err), "enter standing neighbor");
    MapRuntime::FullView full;
    Expect(rt.CollectFullView(819, &full) && full.ready, "full view ready");
    Expect(full.map_instance_id == 21, "full view instance");
    Expect(full.interest.size() == 1 && full.interest[0].player_id == 820, "standing neighbor in full view");
    bool self_in = false;
    for (const auto &e : full.interest) {
        if (e.player_id == 819)
            self_in = true;
    }
    Expect(!self_in, "self not in aoi list");
    Expect(rt.Disconnect(819, &pushes), "disconnect 819");
    MapRuntime::FullView down;
    Expect(rt.CollectFullView(819, &down), "collect while disconnected");
    Expect(!down.ready && std::string(down.step) == "viewer_not_online",
           "disconnected viewer is not an empty view");
    Expect(down.interest.empty(), "not-ready view has no entities");

    // 格子半径为 0 时，AOI 推送看不见邻格，但 32 米内仍要进全量。
    rt.ClearForTest();
    pushes = {};
    Expect(rt.Enter(22, data, Ent(1, 1, 1), &self, &snap, &pushes, &err, 0), "enter radius 0");
    Expect(rt.Enter(22, data, Ent(2, 12, 1), &self, &snap, &pushes, &err, 0), "enter other cell");
    Expect(snap.empty(), "radius 0 enter snap stays empty");
    MapRuntime::FullView near;
    Expect(rt.CollectFullView(1, &near) && near.ready, "32m full view ready");
    Expect(near.interest.size() == 1 && near.interest[0].player_id == 2, "32m neighbor included");

    std::deque<AoiPushBatch> q;
    for (int i = 0; i < 4; ++i) {
        AoiPushBatch b;
        b.map_instance_id = 1;
        AoiRecipientEvent ev;
        ev.op = 2;
        ev.recipient_id = 1;
        ev.snapshot.player_id = static_cast<uint64_t>(i + 1);
        b.events.push_back(ev);
        q.push_back(std::move(b));
    }
    AoiPushBatch reliable;
    reliable.map_instance_id = 1;
    AoiRecipientEvent leave;
    leave.op = 3;
    leave.recipient_id = 9;
    leave.snapshot.player_id = 9;
    reliable.events.push_back(leave);
    q.push_front(std::move(reliable));
    const size_t shed = ShedAoiMoveBatches(&q, 2);
    Expect(shed == 3, "shed oldest move-only batches down to cap");
    Expect(q.size() == 2, "queue stays at cap");
    Expect(!q.empty() && q.front().events.size() == 1 && q.front().events[0].op == 3,
           "ENTER/LEAVE batch is kept");
    std::deque<AoiPushBatch> only_reliable;
    only_reliable.push_back(q.front());
    only_reliable.push_back(q.front());
    only_reliable.push_back(q.front());
    Expect(ShedAoiMoveBatches(&only_reliable, 1) == 0, "reliable batches are not shed");
    Expect(only_reliable.size() == 3, "reliable queue may exceed cap");

    std::vector<AoiMoveCandidate> cands;
    for (uint64_t id = 1; id <= 40; ++id) {
        AoiMoveCandidate c;
        c.entity_id = id;
        c.last_sent_ms = static_cast<int64_t>(id) * 10;
        cands.push_back(c);
    }
    const auto picked = PickAoiMoveEntities(cands, 30);
    Expect(picked.size() == 30, "move frame keeps 30 entities");
    Expect(!picked.empty() && picked[0] == 1, "oldest unsent entity is kept");
    bool kept_newest = false;
    for (uint64_t id : picked) {
        if (id == 40)
            kept_newest = true;
    }
    Expect(!kept_newest, "newest send is left for the next frame");

    rt.ClearForTest();
    pushes = {};
    Expect(rt.Enter(30, data, Ent(1, 1, 1), &self, &snap, &pushes, &err, 2), "budget enter watcher");
    Expect(rt.Enter(30, data, Ent(2, 2, 1), &self, &snap, &pushes, &err, 2), "budget enter near");
    pushes = {};
    Expect(rt.Move(30, 2, 2.2f, 0, 1, 0, 1, 5000, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "near move 1");
    Expect(CountOp(pushes, 1, 2) == 1, "first near move is sent");
    pushes = {};
    Expect(rt.Move(30, 2, 2.4f, 0, 1, 0, 2, 5100, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "near move 2");
    Expect(CountOp(pushes, 1, 2) == 0, "near move inside 200ms is not emitted");
    pushes = {};
    Expect(rt.Move(30, 2, 2.6f, 0, 1, 0, 3, 5200, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "near move 3");
    Expect(CountOp(pushes, 1, 2) == 1, "near move after 200ms is sent");

    rt.ClearForTest();
    pushes = {};
    Expect(rt.Enter(31, data, Ent(1, 1, 1), &self, &snap, &pushes, &err, 3), "budget enter far watcher");
    Expect(rt.Enter(31, data, Ent(2, 25, 1), &self, &snap, &pushes, &err, 3), "budget enter far");
    pushes = {};
    Expect(rt.Move(31, 2, 25.2f, 0, 1, 0, 1, 8000, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "far move 1");
    Expect(CountOp(pushes, 1, 2) == 1, "first far move is sent");
    pushes = {};
    Expect(rt.Move(31, 2, 25.4f, 0, 1, 0, 2, 8200, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "far move 2");
    Expect(CountOp(pushes, 1, 2) == 0, "far move inside 500ms is not emitted");
    pushes = {};
    Expect(rt.Move(31, 2, 25.6f, 0, 1, 0, 3, 8500, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "far move 3");
    Expect(CountOp(pushes, 1, 2) == 1, "far move after 500ms is sent");

    std::unordered_map<uint64_t, int64_t> last_sent;
    std::unordered_map<uint64_t, int> last_round;
    int max_gap = 0;
    for (int round = 0; round < 12; ++round) {
        std::vector<AoiMoveCandidate> crowd;
        crowd.reserve(80);
        for (uint64_t id = 1; id <= 80; ++id) {
            AoiMoveCandidate c;
            c.entity_id = id;
            c.last_sent_ms = last_sent[id];
            crowd.push_back(c);
        }
        const auto turn = PickAoiMoveEntities(std::move(crowd), 40);
        Expect(turn.size() == 40, "80 movers still share 40 slots per frame");
        for (uint64_t id : turn) {
            const auto prev = last_round.find(id);
            if (prev != last_round.end()) {
                const int gap = round - prev->second;
                if (gap > max_gap)
                    max_gap = gap;
            }
            last_round[id] = round;
            last_sent[id] = round + 1;
        }
    }
    int seen = 0;
    for (uint64_t id = 1; id <= 80; ++id) {
        if (last_round.count(id) != 0)
            ++seen;
    }
    Expect(seen == 80, "every one of 80 movers gets a send slot");
    Expect(max_gap > 0 && max_gap <= 2, "no mover waits more than two frames");
    Expect(ServerStats::aoi_lock_samples.load() > lock_samples_before,
           "aoi lock samples increased");
    Expect(ServerStats::aoi_lock_hold_us_sum.load() > lock_hold_before,
           "aoi lock hold time increased");

    if (fails) {
        std::printf("map_runtime_aoi_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK map_runtime_aoi_test\n");
    return 0;
}
