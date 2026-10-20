/**
 * Viewer-centric AOI: a player receives MOVE/ENTER/LEAVE only for targets
 * they subscribe to. The mover's own nearest-N set is not the recipient list.
 */
#include "MapRuntime.h"
#include "MapStaticData.h"

#include <cstdio>
#include <memory>
#include <string>
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

MapEntity Ent(uint64_t id, float x, float z) {
    MapEntity e;
    e.player_id = id;
    e.player_name = "p" + std::to_string(id);
    e.x = x;
    e.y = 0;
    e.z = z;
    e.hp = 100;
    e.max_hp = 100;
    e.move_speed = 10.f;
    e.gateway_instance_id = "gw-0";
    e.session_id = "s" + std::to_string(id);
    return e;
}

bool HasId(const std::vector<MapEntity> &snap, uint64_t id) {
    for (const auto &e : snap) {
        if (e.player_id == id)
            return true;
    }
    return false;
}

int CountSnap(const AoiPushBatch &b, uint64_t recipient, int op, uint64_t subject) {
    int n = 0;
    for (const auto &e : b.events) {
        if (e.recipient_id == recipient && e.op == op && e.snapshot.player_id == subject)
            ++n;
    }
    return n;
}

std::unordered_set<uint64_t> IdsOf(const std::vector<MapEntity> &snap) {
    std::unordered_set<uint64_t> s;
    for (const auto &e : snap)
        s.insert(e.player_id);
    return s;
}

}  // namespace

int main() {
    auto data = MakeOpen();
    if (!data)
        return 1;
    auto &rt = MapRuntime::Instance();
    rt.SetViewRadiusCells(1);
    rt.SetMaxVisible(50);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    MapEntity conf;

    // AOI-01: stacked players. Id 60 is farther in the tie-break than 1..51, so
    // player 1 must not receive ENTER of 60, and 60's own snapshot is 50 people.
    rt.ClearForTest();
    for (uint64_t id = 1; id <= 60; ++id) {
        pushes = {};
        if (!rt.Enter(1, data, Ent(id, 1, 1), &self, &snap, &pushes, &err)) {
            Expect(false, "AOI-01 enter");
            break;
        }
    }
    Expect(snap.size() == 50, "AOI-01 snapshot size 50");
    Expect(CountSnap(pushes, 1, 1, 60) == 0, "AOI-01 unsubscribed viewer gets no ENTER");
    Expect(rt.SnapshotView(1, nullptr, &snap, nullptr) && !HasId(snap, 60) && snap.size() == 50,
           "AOI-01 viewer 1 does not subscribe to 60");

    // AOI-02: MOVE only to current watchers. C is outside the cell radius.
    rt.ClearForTest();
    Expect(rt.Enter(1, data, Ent(1, 1, 1), &self, &snap, &pushes, &err), "AOI-02 enter a");
    Expect(rt.Enter(1, data, Ent(2, 2, 1), &self, &snap, &pushes, &err), "AOI-02 enter b");
    Expect(rt.Enter(1, data, Ent(3, 35, 1), &self, &snap, &pushes, &err), "AOI-02 enter c");
    pushes = {};
    Expect(rt.Move(1, 2, 3, 0, 1, 0, 1, 1000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "AOI-02 move b");
    Expect(CountSnap(pushes, 1, 2, 2) == 1, "AOI-02 watcher receives MOVE");
    Expect(CountSnap(pushes, 3, 2, 2) == 0, "AOI-02 non-watcher receives no MOVE");

    // AOI-03: same cell, increasing distance. Cap keeps the nearer ids.
    rt.ClearForTest();
    Expect(rt.Enter(1, data, Ent(1, 0.2f, 1), &self, &snap, &pushes, &err), "AOI-03 viewer");
    for (uint64_t id = 2; id <= 52; ++id) {
        const float x = 0.2f + static_cast<float>(id) * 0.05f;
        if (!rt.Enter(1, data, Ent(id, x, 1), &self, &snap, &pushes, &err)) {
            Expect(false, "AOI-03 enter");
            break;
        }
    }
    Expect(rt.SnapshotView(1, nullptr, &snap, nullptr), "AOI-03 snap");
    Expect(snap.size() == 50 && HasId(snap, 2) && !HasId(snap, 52),
           "AOI-03 nearer player kept, farthest dropped");

    // AOI-04: re-enter does not leave two copies.
    rt.ClearForTest();
    Expect(rt.Enter(1, data, Ent(1, 1, 1), &self, &snap, &pushes, &err), "AOI-04 first");
    Expect(rt.Enter(1, data, Ent(1, 2, 1), &self, &snap, &pushes, &err), "AOI-04 second");
    Expect(rt.Enter(1, data, Ent(2, 2, 1), &self, &snap, &pushes, &err), "AOI-04 other");
    auto players = rt.ListPlayers();
    int copies = 0;
    for (uint64_t id : players) {
        if (id == 1)
            ++copies;
    }
    Expect(copies == 1 && rt.HasPlayer(1, 1) && !rt.HasPlayer(2, 1), "AOI-04 single entity");
    Expect(rt.SnapshotView(2, nullptr, &snap, nullptr) && HasId(snap, 1) && snap.size() == 1,
           "AOI-04 other sees one copy");

    // AOI-05: after LEAVE, later MOVE is not delivered to the departed player.
    rt.ClearForTest();
    Expect(rt.Enter(1, data, Ent(1, 1, 1), &self, &snap, &pushes, &err), "AOI-05 a");
    Expect(rt.Enter(1, data, Ent(2, 2, 1), &self, &snap, &pushes, &err), "AOI-05 b");
    pushes = {};
    Expect(rt.Leave(1, 2, &pushes), "AOI-05 leave b");
    Expect(CountSnap(pushes, 1, 3, 2) == 1, "AOI-05 watcher receives LEAVE");
    pushes = {};
    Expect(rt.Move(1, 1, 1.5f, 0, 1, 0, 1, 1000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "AOI-05 move a");
    Expect(CountSnap(pushes, 2, 2, 1) == 0, "AOI-05 departed player gets no MOVE");

    // AOI-06: cross-instance transfer drops the old subscription.
    rt.ClearForTest();
    Expect(rt.Enter(1, data, Ent(1, 1, 1), &self, &snap, &pushes, &err), "AOI-06 a map1");
    Expect(rt.Enter(1, data, Ent(2, 2, 1), &self, &snap, &pushes, &err), "AOI-06 b map1");
    pushes = {};
    Expect(rt.Enter(2, data, Ent(1, 1, 1), &self, &snap, &pushes, &err), "AOI-06 a map2");
    Expect(!rt.HasPlayer(1, 1) && rt.HasPlayer(2, 1), "AOI-06 not on both maps");
    Expect(CountSnap(pushes, 2, 3, 1) == 1, "AOI-06 old watcher receives LEAVE");
    Expect(rt.SnapshotView(1, nullptr, &snap, nullptr) && snap.empty(), "AOI-06 new map snapshot empty");
    pushes = {};
    Expect(rt.Move(1, 2, 2.5f, 0, 1, 0, 1, 1000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "AOI-06 move old neighbor");
    Expect(CountSnap(pushes, 1, 2, 2) == 0, "AOI-06 transferred player gets no old-map MOVE");

    // AOI-07: disconnect hides the player; reconnect restores one subscription.
    rt.ClearForTest();
    Expect(rt.Enter(1, data, Ent(1, 1, 1), &self, &snap, &pushes, &err), "AOI-07 a");
    Expect(rt.Enter(1, data, Ent(2, 2, 1), &self, &snap, &pushes, &err), "AOI-07 b");
    pushes = {};
    Expect(rt.Disconnect(1, &pushes), "AOI-07 disconnect a");
    Expect(CountSnap(pushes, 2, 3, 1) == 1, "AOI-07 watcher receives LEAVE");
    pushes = {};
    Expect(rt.Move(1, 2, 2.4f, 0, 1, 0, 1, 1000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "AOI-07 move while a hidden");
    Expect(pushes.events.empty(), "AOI-07 disconnected player gets nothing");
    pushes = {};
    Expect(rt.Reconnect(1, "gw-0", "s1", &self, &snap, &pushes), "AOI-07 reconnect");
    Expect(HasId(snap, 2), "AOI-07 reconnect snapshot has b");
    Expect(CountSnap(pushes, 2, 1, 1) == 1, "AOI-07 watcher receives ENTER again");
    pushes = {};
    Expect(rt.Move(1, 2, 2.6f, 0, 1, 0, 2, 2000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "AOI-07 move after reconnect");
    Expect(CountSnap(pushes, 1, 2, 2) == 1, "AOI-07 restored watcher receives MOVE");

    // AOI-08: equal distance, lower player_id wins. Repeat is identical.
    auto capture = [&](std::unordered_set<uint64_t> *out) {
        rt.ClearForTest();
        for (uint64_t id = 1; id <= 52; ++id)
            rt.Enter(1, data, Ent(id, 1, 1), &self, &snap, &pushes, &err);
        rt.SnapshotView(1, nullptr, &snap, nullptr);
        *out = IdsOf(snap);
    };
    std::unordered_set<uint64_t> a;
    std::unordered_set<uint64_t> b;
    capture(&a);
    capture(&b);
    bool tie = a.size() == 50 && b == a && a.count(2) == 1 && a.count(51) == 1 && a.count(52) == 0;
    Expect(tie, "AOI-08 stable tie-break by player_id");

    if (fails) {
        std::printf("aoi_subscription_invariant_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK aoi_subscription_invariant_test\n");
    return 0;
}
