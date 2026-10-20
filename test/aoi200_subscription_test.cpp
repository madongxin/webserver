/**
 * AOI visible cap 200, shared cell coordinates, and directed subscriptions.
 */
#include "MapRuntime.h"
#include "MapStaticData.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    } else {
        std::printf("PASS: %s\n", m);
    }
}

std::shared_ptr<const MapStaticData> MakeMap(float min_x, float min_z, float max_x, float max_z,
                                            float cell, int grid) {
    const int cells = grid * grid;
    const std::string json =
        std::string("{\"schema_version\":1,\"map_template_id\":9101,\"scene_name\":\"Open\",") +
        "\"data_version\":1,\"bounds_min\":[" + std::to_string(min_x) + ",0," +
        std::to_string(min_z) + "],\"bounds_max\":[" + std::to_string(max_x) + ",5," +
        std::to_string(max_z) + "],\"aoi_cell_size\":" + std::to_string(cell) +
        ",\"nav_sample_step\":1,\"grid_width\":" + std::to_string(grid) +
        ",\"grid_height\":" + std::to_string(grid) + ",\"walkable_rle\":[1," +
        std::to_string(cells) + "],\"spawn_points\":[{\"id\":\"default\",\"position\":[" +
        std::to_string(min_x + 1) + ",0," + std::to_string(min_z + 1) + "],\"yaw\":0}]}";
    std::shared_ptr<const MapStaticData> d;
    std::string err;
    if (!MapStaticData::LoadFromJson(json, &d, &err)) {
        std::printf("FAIL map: %s\n", err.c_str());
        return nullptr;
    }
    return d;
}

MapEntity Ent(uint64_t id, float x, float z) {
    MapEntity e;
    e.player_id = id;
    e.player_name = "p" + std::to_string(id);
    e.x = x;
    e.y = 0;
    e.z = z;
    e.hp = 100;
    e.max_hp = 100;
    e.move_speed = 50.f;
    e.gateway_instance_id = "gw-0";
    e.session_id = "s" + std::to_string(id);
    return e;
}

bool HasId(const std::vector<MapEntity> &snap, uint64_t id) {
    for (const auto &e : snap)
        if (e.player_id == id)
            return true;
    return false;
}

int CountSnap(const AoiPushBatch &b, uint64_t recipient, int op, uint64_t subject) {
    int n = 0;
    for (const auto &e : b.events)
        if (e.recipient_id == recipient && e.op == op && e.snapshot.player_id == subject)
            ++n;
    return n;
}

void Apply(std::unordered_set<uint64_t> *seen, const AoiPushBatch &b, uint64_t self) {
    for (const auto &e : b.events) {
        if (e.recipient_id != self)
            continue;
        if (e.op == 1)
            seen->insert(e.snapshot.player_id);
        else if (e.op == 3)
            seen->erase(e.snapshot.player_id);
    }
}

}  // namespace

int main() {
    auto &rt = MapRuntime::Instance();
    rt.SetMaxVisible(200);
    rt.SetViewRadiusCells(1);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    MapEntity conf;

    auto origin = MakeMap(0, 0, 80, 80, 10, 80);
    auto neg = MakeMap(-1000, -1000, -920, -920, 10, 80);
    auto shifted = MakeMap(500, 800, 580, 880, 10, 80);
    if (!origin || !neg || !shifted)
        return 1;

    // AOI200-01 origin cell.
    rt.ClearForTest();
    Expect(rt.Enter(1, origin, Ent(1, 1, 1), &self, &snap, &pushes, &err, 0), "AOI200-01 a");
    Expect(rt.Enter(1, origin, Ent(2, 2, 1), &self, &snap, &pushes, &err, 0), "AOI200-01 b");
    Expect(snap.size() == 1 && snap[0].player_id == 1, "AOI200-01 same cell");

    // AOI200-02 negative origin. WorldToAoiCell cell 0, floor(x/cell) is not.
    rt.ClearForTest();
    pushes = {};
    Expect(rt.Enter(2, neg, Ent(1, -995, -995), &self, &snap, &pushes, &err, 0), "AOI200-02 a");
    Expect(rt.Enter(2, neg, Ent(2, -994, -995), &self, &snap, &pushes, &err, 0), "AOI200-02 b");
    Expect(snap.size() == 1, "AOI200-02 negative origin same cell");
    Expect(CountSnap(pushes, 1, 1, 2) == 1, "AOI200-02 watcher ENTER");

    // AOI200-03 positive shifted origin.
    rt.ClearForTest();
    pushes = {};
    Expect(rt.Enter(3, shifted, Ent(1, 501, 801), &self, &snap, &pushes, &err, 0), "AOI200-03 a");
    Expect(rt.Enter(3, shifted, Ent(2, 502, 801), &self, &snap, &pushes, &err, 0), "AOI200-03 b");
    Expect(snap.size() == 1 && CountSnap(pushes, 1, 1, 2) == 1, "AOI200-03 shifted origin");

    // AOI200-04 cell boundary.
    rt.ClearForTest();
    Expect(rt.Enter(4, origin, Ent(1, 9.9f, 1), &self, &snap, &pushes, &err, 0), "AOI200-04 a");
    Expect(rt.Enter(4, origin, Ent(2, 10.1f, 1), &self, &snap, &pushes, &err, 0), "AOI200-04 b");
    Expect(snap.empty(), "AOI200-04 radius 0 stops at the cell edge");
    Expect(rt.Enter(4, origin, Ent(3, 9.2f, 1), &self, &snap, &pushes, &err, 0), "AOI200-04 c");
    Expect(HasId(snap, 1) && !HasId(snap, 2), "AOI200-04 same cell kept, next cell out");

    // AOI200-05 radius 0 same cell.
    rt.ClearForTest();
    Expect(rt.Enter(5, origin, Ent(1, 4, 4), &self, &snap, &pushes, &err, 0), "AOI200-05 a");
    Expect(rt.Enter(5, origin, Ent(2, 5, 4), &self, &snap, &pushes, &err, 0), "AOI200-05 b");
    Expect(snap.size() == 1, "AOI200-05 radius 0 sees the current cell");

    // AOI200-06 cross cell then snapshot matches events.
    rt.ClearForTest();
    rt.SetViewRadiusCells(1);
    std::unordered_set<uint64_t> sim;
    Expect(rt.Enter(6, origin, Ent(1, 1, 1), &self, &snap, &pushes, &err), "AOI200-06 a");
    pushes = {};
    Expect(rt.Enter(6, origin, Ent(2, 35, 1), &self, &snap, &pushes, &err), "AOI200-06 b far");
    Apply(&sim, pushes, 1);
    Expect(sim.empty(), "AOI200-06 far player is not subscribed");
    pushes = {};
    Expect(rt.Move(6, 2, 2, 0, 1, 0, 1, 1000, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "AOI200-06 move into view");
    Apply(&sim, pushes, 1);
    Expect(rt.SnapshotView(1, nullptr, &snap, nullptr), "AOI200-06 snap");
    Expect(sim.count(2) == 1 && HasId(snap, 2) && snap.size() == sim.size(),
           "AOI200-06 events converge to snapshot");

    rt.ClearForTest();
    rt.SetMaxVisible(200);
    rt.SetViewRadiusCells(1);
    bool cap_ok = true;
    for (int i = 1; i <= 201; ++i) {
        pushes = {};
        if (!rt.Enter(7, origin, Ent(static_cast<uint64_t>(i), 1, 1), &self, &snap, &pushes, &err))
            cap_ok = false;
        if (snap.size() > 200)
            cap_ok = false;
        std::unordered_set<uint64_t> ids;
        for (const auto &e : snap)
            ids.insert(e.player_id);
        if (ids.size() != snap.size())
            cap_ok = false;
    }
    Expect(cap_ok && snap.size() == 200, "AOI200-07 201 players cap and no duplicate");

    rt.ClearForTest();
    for (int i = 1; i <= 250; ++i)
        rt.Enter(8, origin, Ent(static_cast<uint64_t>(i), 1, 1), &self, &snap, &pushes, &err);
    Expect(rt.SnapshotView(1, nullptr, &snap, nullptr) && snap.size() == 200 && HasId(snap, 2) &&
               HasId(snap, 201) && !HasId(snap, 250),
           "AOI200-08 tie-break keeps lower ids");
    std::vector<MapEntity> again;
    rt.SnapshotView(1, nullptr, &again, nullptr);
    Expect(again.size() == snap.size(), "AOI200-08 stable repeat");

    rt.ClearForTest();
    for (int i = 1; i <= 400; ++i)
        rt.Enter(9, origin, Ent(static_cast<uint64_t>(i), 1, 1), &self, &snap, &pushes, &err);
    Expect(rt.SnapshotView(1, nullptr, &snap, nullptr) && snap.size() == 200 && !HasId(snap, 400),
           "AOI200-09 viewer cap in a 400 stack");
    pushes = {};
    Expect(rt.Leave(9, 2, &pushes), "AOI200-09 leave");
    Expect(CountSnap(pushes, 1, 3, 2) == 1, "AOI200-09 watcher receives LEAVE");
    Expect(CountSnap(pushes, 1, 1, 202) == 1, "AOI200-09 next id receives ENTER");
    Expect(CountSnap(pushes, 400, 2, 1) == 0, "AOI200-09 unsubscribed player gets no MOVE");
    Expect(rt.SnapshotView(1, nullptr, &snap, nullptr) && !HasId(snap, 2) && HasId(snap, 202) &&
               snap.size() == 200,
           "AOI200-09 snapshot follows the promotion");

    // AOI200-10 fixed-seed moves. 80 players keeps the checker on this 4-core host.
    rt.ClearForTest();
    const int pop = 80;
    std::vector<std::unordered_set<uint64_t>> client(static_cast<size_t>(pop) + 1);
    for (int i = 1; i <= pop; ++i) {
        pushes = {};
        const float x = 1.f + static_cast<float>((i * 3) % 20);
        const float z = 1.f + static_cast<float>((i * 5) % 20);
        rt.Enter(10, origin, Ent(static_cast<uint64_t>(i), x, z), &self, &snap, &pushes, &err);
        for (const auto &e : snap)
            client[static_cast<size_t>(i)].insert(e.player_id);
        for (int v = 1; v <= i; ++v)
            Apply(&client[static_cast<size_t>(v)], pushes, static_cast<uint64_t>(v));
    }
    uint64_t seed = 200;
    for (int step = 0; step < 12; ++step) {
        seed = seed * 6364136223846793005ULL + 1;
        const uint64_t who = 1 + (seed % static_cast<uint64_t>(pop));
        const float x = 1.f + static_cast<float>((seed >> 8) % 20);
        const float z = 1.f + static_cast<float>((seed >> 16) % 20);
        pushes = {};
        rt.Move(10, who, x, 0, z, 0, static_cast<uint64_t>(step + 1), 1000 + step * 200, &conf,
                &pushes, &err, true);
        for (int v = 1; v <= pop; ++v)
            Apply(&client[static_cast<size_t>(v)], pushes, static_cast<uint64_t>(v));
    }
    bool converged = true;
    for (int v = 1; v <= pop; ++v) {
        if (!rt.SnapshotView(static_cast<uint64_t>(v), nullptr, &snap, nullptr))
            converged = false;
        if (snap.size() > 200)
            converged = false;
        std::unordered_set<uint64_t> got;
        for (const auto &e : snap)
            got.insert(e.player_id);
        if (got != client[static_cast<size_t>(v)])
            converged = false;
    }
    Expect(converged, "AOI200-10 client set matches snapshot");

    // AOI200-11 reconnect replaces the session used for later pushes.
    rt.ClearForTest();
    Expect(rt.Enter(11, origin, Ent(1, 1, 1), &self, &snap, &pushes, &err), "AOI200-11 a");
    Expect(rt.Enter(11, origin, Ent(2, 2, 1), &self, &snap, &pushes, &err), "AOI200-11 b");
    pushes = {};
    Expect(rt.Disconnect(1, &pushes), "AOI200-11 disconnect");
    Expect(CountSnap(pushes, 2, 3, 1) == 1, "AOI200-11 LEAVE");
    pushes = {};
    Expect(rt.Reconnect(1, "gw-1", "s1-new", &self, &snap, &pushes), "AOI200-11 reconnect");
    Expect(HasId(snap, 2), "AOI200-11 snapshot restored");
    pushes = {};
    Expect(rt.Move(11, 2, 2.4f, 0, 1, 0, 1, 2000, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "AOI200-11 move");
    bool new_session = false;
    bool old_session = false;
    for (const auto &e : pushes.events) {
        if (e.recipient_id == 1 && e.op == 2) {
            if (e.session_id == "s1-new" && e.gateway_instance_id == "gw-1")
                new_session = true;
            if (e.session_id == "s1")
                old_session = true;
        }
    }
    Expect(new_session && !old_session, "AOI200-11 old session is not addressed");

    // AOI200-12 cross map.
    rt.ClearForTest();
    Expect(rt.Enter(12, origin, Ent(1, 1, 1), &self, &snap, &pushes, &err), "AOI200-12 a");
    Expect(rt.Enter(12, origin, Ent(2, 2, 1), &self, &snap, &pushes, &err), "AOI200-12 b");
    pushes = {};
    Expect(rt.Enter(13, origin, Ent(1, 3, 1), &self, &snap, &pushes, &err), "AOI200-12 transfer");
    Expect(!rt.HasPlayer(12, 1) && rt.HasPlayer(13, 1) && rt.PlayerMap(1) == 13,
           "AOI200-12 one map");
    Expect(CountSnap(pushes, 2, 3, 1) == 1, "AOI200-12 old watcher LEAVE");
    Expect(snap.size() <= 200, "AOI200-12 new snapshot capped");

    // Different instances move without sharing the registry lock for the scan.
    rt.ClearForTest();
    for (int map = 1; map <= 4; ++map) {
        for (int i = 1; i <= 20; ++i) {
            const uint64_t id = static_cast<uint64_t>(map) * 1000 + static_cast<uint64_t>(i);
            rt.Enter(static_cast<uint64_t>(map), origin, Ent(id, 1.f + i, 1), &self, &snap,
                     &pushes, &err);
        }
    }
    std::atomic<int> move_ok{0};
    std::vector<std::thread> threads;
    for (int map = 1; map <= 4; ++map) {
        threads.emplace_back([&, map]() {
            MapEntity local;
            AoiPushBatch batch;
            std::string local_err;
            for (int step = 0; step < 30; ++step) {
                const uint64_t id = static_cast<uint64_t>(map) * 1000 + 1;
                const float x = 1.f + static_cast<float>(step % 8);
                if (rt.Move(static_cast<uint64_t>(map), id, x, 0, 1, 0,
                            static_cast<uint64_t>(step + 1), 1000 + step * 100, &local, &batch,
                            &local_err, true) == MapMoveReject::Ok)
                    move_ok.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &th : threads)
        th.join();
    Expect(move_ok.load() == 120, "AOI200 parallel moves on four instances");

    if (fails) {
        std::printf("aoi200_subscription_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK aoi200_subscription_test\n");
    return 0;
}
