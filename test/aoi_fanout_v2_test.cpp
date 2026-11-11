#include "AoiFanoutV2.h"
#include "AoiMoveCoalesceLedger.h"
#include "MapRuntime.h"
#include "MapStaticData.h"
#include "MoveSendConfig.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++g_fail;
    }
}

int CountOp(const std::vector<AoiPushBatch> &batches, uint64_t recipient, int op) {
    int n = 0;
    for (const auto &b : batches) {
        for (const auto &e : b.events) {
            if (e.recipient_id == recipient && e.op == op)
                ++n;
        }
    }
    return n;
}

std::shared_ptr<const MapStaticData> MakeOpen() {
    const std::string json = R"JSON({
      "schema_version": 1, "map_template_id": 9101, "scene_name": "Open", "data_version": 1,
      "bounds_min": [0,0,0], "bounds_max": [80,5,40], "aoi_cell_size": 10.0, "nav_sample_step": 1.0,
      "grid_width": 80, "grid_height": 40, "walkable_rle": [1, 3200],
      "spawn_points": [{"id":"default","position":[1,0,1],"yaw":0}]
    })JSON";
    std::shared_ptr<const MapStaticData> d;
    std::string err;
    if (!MapStaticData::LoadFromJson(json, &d, &err)) {
        std::printf("FAIL map %s\n", err.c_str());
        return nullptr;
    }
    return d;
}

MapEntity Ent(uint64_t id, float x, float z) {
    MapEntity e;
    e.player_id = id;
    e.player_name = "p";
    e.x = x;
    e.y = 0;
    e.z = z;
    e.hp = 100;
    e.max_hp = 100;
    e.move_speed = 10.f;
    e.gateway_instance_id = "gw";
    e.session_id = "s";
    return e;
}

void TestCapDoesNotFlap() {
    const int cap = 4;
    const int slack = 2;
    std::vector<AoiCand> cands;
    for (int i = 1; i <= 6; ++i) {
        AoiCand c;
        c.id = static_cast<uint64_t>(i);
        c.d2 = static_cast<float>(i);
        c.in_enter = true;
        c.in_leave = true;
        c.in_notified = i <= cap;
        cands.push_back(c);
    }
    auto first = SelectVisible(cands, cap, slack);
    Expect(static_cast<int>(first.size()) <= cap, "visible count stays within the cap");
    Expect(static_cast<int>(first.size()) == cap, "filled to the cap");
    std::vector<AoiCand> fresh;
    for (int i = 1; i <= 6; ++i) {
        AoiCand c;
        c.id = static_cast<uint64_t>(i);
        c.d2 = static_cast<float>(i);
        c.in_enter = true;
        c.in_leave = true;
        fresh.push_back(c);
    }
    auto filled = SelectVisible(fresh, cap, slack);
    Expect(static_cast<int>(filled.size()) == cap, "a short set fills up to the cap");
    for (auto &c : cands) {
        if (c.id == 4)
            c.d2 = 5.2f;
        if (c.id == 5)
            c.d2 = 4.2f;
    }
    for (auto &c : cands)
        c.in_notified = false;
    for (uint64_t id : first) {
        for (auto &c : cands) {
            if (c.id == id)
                c.in_notified = true;
        }
    }
    auto second = SelectVisible(cands, cap, slack);
    int changes = 0;
    for (uint64_t id : first) {
        bool still = false;
        for (uint64_t other : second) {
            if (other == id)
                still = true;
        }
        if (!still)
            ++changes;
    }
    for (uint64_t id : second) {
        bool was = false;
        for (uint64_t other : first) {
            if (other == id)
                was = true;
        }
        if (!was)
            ++changes;
    }
    Expect(changes == 0, "rank N and N+1 inside slack do not enter and leave");
}

void TestHysteresisBand() {
    std::vector<AoiCand> outside;
    AoiCand far;
    far.id = 2;
    far.d2 = 100;
    far.in_enter = false;
    far.in_leave = true;
    far.in_notified = false;
    outside.push_back(far);
    auto none = SelectVisible(outside, 10, 0);
    Expect(none.empty(), "standing in the margin does not enter");
    far.in_enter = true;
    far.in_notified = false;
    std::vector<AoiCand> inner{far};
    auto entered = SelectVisible(inner, 10, 0);
    Expect(entered.size() == 1, "inside the enter radius is selected");
    far.in_enter = false;
    far.in_leave = true;
    far.in_notified = true;
    std::vector<AoiCand> band{far};
    auto held = SelectVisible(band, 10, 0);
    Expect(held.size() == 1, "margin keeps an already visible entity");
    int flips = 0;
    bool notified = true;
    for (int i = 0; i < 20; ++i) {
        far.in_enter = (i % 2) == 0;
        far.in_leave = true;
        far.in_notified = notified;
        auto now = SelectVisible(std::vector<AoiCand>{far}, 10, 0);
        const bool on = !now.empty();
        if (on != notified)
            ++flips;
        notified = on;
    }
    Expect(flips == 0, "wiggling inside the leave radius does not flap");
}

void TestBudgetKeepsFarBehind() {
    ::setenv("GAMEMESH_AOI_V2_LAYERS", "1", 1);
    AoiMoveCoalesceLedger ledger;
    ledger.SetEnforceByteBudget(true);
    const int64_t t0 = 10000;
    for (int id = 1; id <= 4; ++id)
        ledger.Offer(static_cast<uint64_t>(id), 1, t0, 1);
    for (int id = 5; id <= 34; ++id)
        ledger.Offer(static_cast<uint64_t>(id), 1, t0, 0);
    int near_n = 0;
    int far_n = 0;
    int bytes = 0;
    for (int step = 0; step < 5; ++step) {
        int64_t wait = 0;
        const int64_t now = t0 + step * 1000;
        for (int id = 1; id <= 4; ++id)
            ledger.Offer(static_cast<uint64_t>(id), static_cast<uint64_t>(step + 2), now, 1);
        for (int id = 5; id <= 34; ++id)
            ledger.Offer(static_cast<uint64_t>(id), static_cast<uint64_t>(step + 2), now, 0);
        auto picked = ledger.Pick(now, 331, 52, 200, &wait);
        int frame_near = 0;
        int frame_far = 0;
        for (const auto &s : picked) {
            if (s.entity_id <= 4)
                ++frame_near;
            else
                ++frame_far;
        }
        near_n += frame_near;
        far_n += frame_far;
        if (!picked.empty()) {
            ledger.NoteParked(picked);
            ledger.NoteKernelWrite(picked, now, static_cast<int>(picked.size()) * 52);
            bytes += static_cast<int>(picked.size()) * 52 + 66;
        }
    }
    Expect(far_n <= near_n, "far layer does not outrun the near layer");
    Expect(far_n < 30, "far layer is not flushed every second");
    Expect(bytes <= 331 * 5, "charged bytes stay inside the per-connection budget");
    Expect(near_n > 0, "near layer still gets slots");
    ::unsetenv("GAMEMESH_AOI_V2_LAYERS");
}

void TestLeaveStaysUntilWrite() {
    ::setenv("GAMEMESH_AOI_V2_VISIBLE_SET", "1", 1);
    auto data = MakeOpen();
    if (!data) {
        ++g_fail;
        return;
    }
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetViewRadiusCells(1);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(1, data, Ent(1, 1, 1), &self, &snap, &pushes, &err), "enter watcher");
    Expect(rt.Enter(1, data, Ent(2, 2, 1), &self, &snap, &pushes, &err), "enter subject");
    pushes = {};
    MapEntity conf;
    Expect(rt.Move(1, 2, 35, 0, 1, 0, 1, 1000, &conf, &pushes, &err) == MapMoveReject::Ok, "move away");
    int leaves = 0;
    for (int i = 0; i < 30 && leaves == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        leaves += CountOp(more, 1, 3);
    }
    Expect(leaves >= 1, "leaving the window emits LEAVE");
    Expect(rt.LeavePendingCountForTest(1) >= 1, "undelivered LEAVE stays pending");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::vector<AoiPushBatch> again;
    rt.AdvancePresented(&again);
    Expect(CountOp(again, 1, 3) >= 1, "LEAVE is sent again after the old retry window");
    uint64_t version = 0;
    for (const auto &b : again) {
        for (const auto &e : b.events) {
            if (e.op == 3 && e.recipient_id == 1)
                version = e.notify_version;
        }
    }
    rt.NoteAoiWrite(1, 2, 3, true, version);
    Expect(rt.LeavePendingCountForTest(1) == 0, "gateway write clears the pending LEAVE");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::vector<AoiPushBatch> done;
    rt.AdvancePresented(&done);
    Expect(CountOp(done, 1, 3) == 0, "delivered LEAVE is not repeated");
    ::unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
    rt.ClearForTest();
}

void TestResyncDiff() {
    std::vector<AoiCand> cands;
    for (int id = 1; id <= 3; ++id) {
        AoiCand c;
        c.id = static_cast<uint64_t>(id);
        c.d2 = 1000;
        c.in_notified = true;
        c.in_leave = false;
        cands.push_back(c);
    }
    for (int id = 4; id <= 5; ++id) {
        AoiCand c;
        c.id = static_cast<uint64_t>(id);
        c.d2 = static_cast<float>(id);
        c.in_enter = true;
        c.in_leave = true;
        c.in_notified = false;
        cands.push_back(c);
    }
    auto desired = SelectVisible(cands, 10, 0);
    int leaves = 0;
    int enters = 0;
    for (int id = 1; id <= 3; ++id) {
        bool keep = false;
        for (uint64_t d : desired) {
            if (d == static_cast<uint64_t>(id))
                keep = true;
        }
        if (!keep)
            ++leaves;
    }
    for (uint64_t d : desired) {
        if (d == 4 || d == 5)
            ++enters;
    }
    Expect(leaves == 3, "entities outside the window are left");
    Expect(enters == 2, "current window members are entered");
}

void Pump(MapRuntime &rt, int n, uint64_t viewer, int *enters, int *leaves) {
    for (int i = 0; i < n; ++i) {
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        if (enters)
            *enters += CountOp(more, viewer, 1);
        if (leaves)
            *leaves += CountOp(more, viewer, 3);
    }
}

void TestHysteresisOnMap() {
    ::setenv("GAMEMESH_AOI_V2_VISIBLE_SET", "1", 1);
    ::setenv("GAMEMESH_AOI_V2_HYSTERESIS", "1", 1);
    auto data = MakeOpen();
    if (!data) {
        ++g_fail;
        ::unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
        ::unsetenv("GAMEMESH_AOI_V2_HYSTERESIS");
        return;
    }
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetViewRadiusCells(1);
    rt.SetMaxVisible(200);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(9102, data, Ent(11, 5, 5), &self, &snap, &pushes, &err), "hysteresis watcher");
    pushes = {};
    Expect(rt.Enter(9102, data, Ent(12, 6, 5), &self, &snap, &pushes, &err), "hysteresis subject");
    int enters = CountOp(std::vector<AoiPushBatch>{pushes}, 11, 1);
    int leaves = CountOp(std::vector<AoiPushBatch>{pushes}, 11, 3);
    MapEntity conf;
    pushes = {};
    Expect(rt.Move(9102, 12, 25, 0, 5, 0, 1, 2000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "step into the margin");
    enters += CountOp(std::vector<AoiPushBatch>{pushes}, 11, 1);
    leaves += CountOp(std::vector<AoiPushBatch>{pushes}, 11, 3);
    Pump(rt, 15, 11, &enters, &leaves);
    for (int i = 0; i < 20; ++i) {
        const float x = (i % 2) == 0 ? 15.f : 25.f;
        pushes = {};
        Expect(rt.Move(9102, 12, x, 0, 5, 0, static_cast<uint64_t>(i + 2),
                       3000 + static_cast<int64_t>(i) * 1000, &conf, &pushes, &err) ==
                   MapMoveReject::Ok,
               "wiggle inside the outer radius");
        enters += CountOp(std::vector<AoiPushBatch>{pushes}, 11, 1);
        leaves += CountOp(std::vector<AoiPushBatch>{pushes}, 11, 3);
        Pump(rt, 8, 11, &enters, &leaves);
    }
    Expect(enters == 1, "crossing the inner edge does not enter again");
    Expect(leaves == 0, "staying inside the outer radius does not leave");
    pushes = {};
    Expect(rt.Move(9102, 12, 45, 0, 5, 0, 40, 30000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "step outside the outer radius");
    leaves += CountOp(std::vector<AoiPushBatch>{pushes}, 11, 3);
    Pump(rt, 20, 11, &enters, &leaves);
    Expect(leaves >= 1, "leaving the outer radius emits LEAVE");
    ::unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
    ::unsetenv("GAMEMESH_AOI_V2_HYSTERESIS");
    rt.ClearForTest();
}

void TestCrowdCap() {
    ::setenv("GAMEMESH_AOI_V2_VISIBLE_SET", "1", 1);
    ::setenv("GAMEMESH_AOI_V2_CAP_SLACK", "0", 1);
    auto data = MakeOpen();
    if (!data) {
        ++g_fail;
        ::unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
        ::unsetenv("GAMEMESH_AOI_V2_CAP_SLACK");
        return;
    }
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetMaxVisible(4);
    rt.SetViewRadiusCells(2);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(9103, data, Ent(30, 1, 1), &self, &snap, &pushes, &err), "crowd watcher");
    std::vector<uint64_t> seen;
    auto note = [&](const AoiPushBatch &b) {
        for (const auto &e : b.events) {
            if (e.recipient_id != 30 || e.op != 1)
                continue;
            seen.push_back(e.snapshot.player_id);
        }
        for (const auto &e : b.events) {
            if (e.recipient_id != 30 || e.op != 3)
                continue;
            seen.erase(std::remove(seen.begin(), seen.end(), e.snapshot.player_id), seen.end());
        }
    };
    for (int i = 1; i <= 6; ++i) {
        pushes = {};
        Expect(rt.Enter(9103, data, Ent(static_cast<uint64_t>(30 + i), 1.f + static_cast<float>(i), 1),
                        &self, &snap, &pushes, &err),
               "crowd member");
        note(pushes);
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        for (const auto &b : more)
            note(b);
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    Expect(static_cast<int>(seen.size()) <= 4, "visible set stays within the cap");
    Expect(static_cast<int>(seen.size()) == 4, "visible set fills the cap");
    rt.SetMaxVisible(200);
    ::unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
    ::unsetenv("GAMEMESH_AOI_V2_CAP_SLACK");
    rt.ClearForTest();
}

void TestRadiusFrozenAtEnter() {
    ::setenv("GAMEMESH_AOI_V2_RADIUS_CELLS", "1", 1);
    auto data = MakeOpen();
    if (!data) {
        ++g_fail;
        ::unsetenv("GAMEMESH_AOI_V2_RADIUS_CELLS");
        return;
    }
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetViewRadiusCells(2);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(9104, data, Ent(40, 1, 1), &self, &snap, &pushes, &err), "radius enter");
    MapRuntime::FullView view;
    Expect(rt.CollectFullView(40, &view), "radius view");
    Expect(view.radius_cells == 1, "env radius is copied onto the new instance");
    ::unsetenv("GAMEMESH_AOI_V2_RADIUS_CELLS");
    Expect(rt.Enter(9104, data, Ent(41, 2, 1), &self, &snap, &pushes, &err), "second enter");
    Expect(rt.CollectFullView(41, &view), "second view");
    Expect(view.radius_cells == 1, "clearing the env leaves the existing instance radius");
    Expect(rt.Enter(9105, data, Ent(42, 1, 1), &self, &snap, &pushes, &err), "new map");
    Expect(rt.CollectFullView(42, &view), "new map view");
    Expect(view.radius_cells == 2, "a new instance uses the configured radius");
    rt.ClearForTest();
}

void TestLeaveRetriesPastTheOldCap() {
    ::unsetenv("GAMEMESH_AOI_V2_VISIBLE_SET");
    auto data = MakeOpen();
    if (!data) {
        ++g_fail;
        return;
    }
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    rt.SetViewRadiusCells(1);
    rt.SetMaxVisible(200);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(9106, data, Ent(61, 1, 1), &self, &snap, &pushes, &err), "retry watcher");
    Expect(rt.Enter(9106, data, Ent(62, 2, 1), &self, &snap, &pushes, &err), "retry subject");
    MapEntity conf;
    pushes = {};
    Expect(rt.Move(9106, 62, 35, 0, 1, 0, 1, 1000, &conf, &pushes, &err) == MapMoveReject::Ok,
           "retry move away");
    int leaves = CountOp(std::vector<AoiPushBatch>{pushes}, 61, 3);
    uint64_t version = 0;
    for (int i = 0; i < 40 && leaves == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        leaves += CountOp(more, 61, 3);
        for (const auto &b : more) {
            for (const auto &e : b.events) {
                if (e.op == 3 && e.recipient_id == 61)
                    version = e.notify_version;
            }
        }
    }
    Expect(leaves >= 1, "leaving the window emits LEAVE");
    Expect(version != 0, "LEAVE carries a write version");
    int extra = 0;
    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        std::vector<AoiPushBatch> more;
        rt.AdvancePresented(&more);
        extra += CountOp(more, 61, 3);
    }
    Expect(extra >= 1, "an unacked LEAVE is retried");
    Expect(extra <= 3, "LEAVE retries stay bounded");
    rt.NoteAoiWrite(61, 62, 3, true, version);
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    std::vector<AoiPushBatch> done;
    rt.AdvancePresented(&done);
    Expect(CountOp(done, 61, 3) == 0, "gateway write stops further LEAVE");
    rt.ClearForTest();
}

}  // namespace

int main() {
    TestCapDoesNotFlap();
    TestHysteresisBand();
    TestBudgetKeepsFarBehind();
    TestResyncDiff();
    TestLeaveStaysUntilWrite();
    TestHysteresisOnMap();
    TestCrowdCap();
    TestRadiusFrozenAtEnter();
    TestLeaveRetriesPastTheOldCap();
    if (g_fail) {
        std::printf("aoi_fanout_v2_test FAIL count=%d\n", g_fail);
        return 1;
    }
    std::printf("OK aoi_fanout_v2_test\n");
    return 0;
}
