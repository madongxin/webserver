#include "AoiEvidence.h"
#include "Logging.h"
#include "MapRuntime.h"
#include "MapStaticData.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int fails = 0;

void Expect(bool c, const char *m) {
    if (!c) {
        std::cerr << "FAIL: " << m << "\n";
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
      "bounds_max": [80, 5, 80],
      "aoi_cell_size": 10.0,
      "nav_sample_step": 1.0,
      "grid_width": 80,
      "grid_height": 80,
      "walkable_rle": [1, 6400],
      "spawn_points": [{"id":"default","position":[5, 0, 5],"yaw":0}]
    })JSON";
    std::shared_ptr<const MapStaticData> d;
    std::string err;
    if (!MapStaticData::LoadFromJson(json, &d, &err)) {
        std::cerr << "FAIL make open: " << err << "\n";
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
    e.move_speed = 400.f;
    e.gateway_instance_id = "gw-0";
    e.session_id = "s" + std::to_string(id);
    return e;
}

void Collect(const std::vector<AoiPushBatch> &batches, AoiPushBatch *out) {
    for (const auto &b : batches) {
        for (const auto &ev : b.events)
            out->events.push_back(ev);
    }
}

int FailLeaves(MapRuntime *rt, const AoiPushBatch &batch, uint64_t viewer_lo, uint64_t viewer_hi) {
    int n = 0;
    for (const auto &ev : batch.events) {
        if (ev.op != 3 || ev.recipient_id < viewer_lo || ev.recipient_id > viewer_hi)
            continue;
        if (ev.snapshot.player_id == 0 || ev.notify_version == 0)
            continue;
        rt->NoteAoiWrite(ev.recipient_id, ev.snapshot.player_id, 3, false, ev.notify_version);
        ++n;
    }
    return n;
}

int CountLeaveTo(const AoiPushBatch &batch, uint64_t viewer_lo, uint64_t viewer_hi) {
    int n = 0;
    for (const auto &ev : batch.events) {
        if (ev.op == 3 && ev.recipient_id >= viewer_lo && ev.recipient_id <= viewer_hi)
            ++n;
    }
    return n;
}

bool KnowOnline(uint64_t id, bool *online) {
    if (!online)
        return false;
    *online = MapRuntime::Instance().ViewerConnected(id);
    return true;
}

void TestDisconnectClearsPending(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    AoiResetEvidenceForTest();
    AoiSetViewerOnlineFn(KnowOnline);
    rt.SetViewRadiusCells(1);
    rt.SetMaxVisible(50);
    MapRuntime::SetNowMsForTest(5000);
    AoiSetEvidenceNowMsForTest(5000);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(1, data, Ent(1, 5.f, 5.f), &self, &snap, &pushes, &err), "a enter viewer");
    Expect(rt.Enter(1, data, Ent(2, 6.f, 5.f), &self, &snap, &pushes, &err), "a enter subject");
    MapEntity conf;
    Expect(rt.Move(1, 2, 70.f, 0.f, 5.f, 0.f, 1, 5000, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "a move out");
    std::vector<AoiPushBatch> more;
    rt.AdvancePresented(&more);
    AoiPushBatch all;
    Collect(more, &all);
    const int noted = FailLeaves(&rt, all, 1, 1);
    Expect(noted >= 1, "a leave was written");
    Expect(AoiLeavePendingForViewer(1) == 1, "a pending recorded");
    Expect(rt.Disconnect(1, &pushes), "a disconnect");
    more.clear();
    rt.AdvancePresented(&more);
    all = {};
    Collect(more, &all);
    Expect(AoiLeavePendingForViewer(1) == 0, "a pending cleared");
    Expect(AoiLeavePendingCount() == 0, "a pending metric zero");
    Expect(CountLeaveTo(all, 1, 1) == 0, "a no resend");
    Expect(AoiLeaveDroppedOfflineCount() >= 1, "a dropped offline counter");
}

void TestManyViewersDrop(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    AoiResetEvidenceForTest();
    rt.SetViewRadiusCells(1);
    rt.SetMaxVisible(400);
    MapRuntime::SetNowMsForTest(8000);
    AoiSetEvidenceNowMsForTest(8000);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    for (uint64_t v = 1; v <= 100; ++v) {
        if (!rt.Enter(1, data, Ent(v, 5.f, 5.f), &self, &snap, &pushes, &err, 1, 400)) {
            Expect(false, "b enter viewer");
            return;
        }
    }
    for (uint64_t s = 101; s <= 300; ++s) {
        if (!rt.Enter(1, data, Ent(s, 6.f, 5.f), &self, &snap, &pushes, &err, 1, 400)) {
            Expect(false, "b enter subject");
            return;
        }
    }
    int noted = 0;
    for (uint64_t s = 101; s <= 300; ++s) {
        MapEntity conf;
        if (rt.Move(1, s, 70.f, 0.f, 5.f, 0.f, 1, 8000, &conf, &pushes, &err, true) != MapMoveReject::Ok) {
            Expect(false, "b move");
            return;
        }
    }
    std::vector<AoiPushBatch> more;
    rt.AdvancePresented(&more);
    AoiPushBatch all;
    Collect(more, &all);
    noted += FailLeaves(&rt, all, 1, 100);
    Expect(noted >= 100, "b enough failed leaves");
    Expect(AoiLeavePendingCount() >= 100, "b pending recorded");
    for (uint64_t v = 1; v <= 100; ++v)
        rt.Disconnect(v, &pushes);
    more.clear();
    rt.AdvancePresented(&more);
    all = {};
    Collect(more, &all);
    Expect(AoiLeavePendingCount() == 0, "b pending zero");
    int resent = CountLeaveTo(all, 1, 100);
    MapRuntime::SetNowMsForTest(8000 + 60000);
    AoiSetEvidenceNowMsForTest(8000 + 60000);
    more.clear();
    rt.AdvancePresented(&more);
    all = {};
    Collect(more, &all);
    resent += CountLeaveTo(all, 1, 100);
    Expect(resent == 0, "b no new retry");
}

void TestGiveUpUsesFirstFail(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    AoiResetEvidenceForTest();
    rt.SetViewRadiusCells(1);
    rt.SetMaxVisible(50);
    const int64_t t0 = 1000000;
    MapRuntime::SetNowMsForTest(t0);
    AoiSetEvidenceNowMsForTest(t0);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(1, data, Ent(1, 5.f, 5.f), &self, &snap, &pushes, &err), "c enter viewer");
    Expect(rt.Enter(1, data, Ent(2, 6.f, 5.f), &self, &snap, &pushes, &err), "c enter subject");
    MapEntity conf;
    Expect(rt.Move(1, 2, 70.f, 0.f, 5.f, 0.f, 1, t0, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "c move out");
    std::vector<AoiPushBatch> more;
    rt.AdvancePresented(&more);
    AoiPushBatch all;
    Collect(more, &all);
    Expect(FailLeaves(&rt, all, 1, 1) >= 1, "c leave failed");
    Expect(AoiLeavePendingCount() == 1, "c pending one");
    const int64_t cap = 600000;
    MapRuntime::SetNowMsForTest(t0 + cap - 1);
    AoiSetEvidenceNowMsForTest(t0 + cap - 1);
    more.clear();
    rt.AdvancePresented(&more);
    all = {};
    Collect(more, &all);
    FailLeaves(&rt, all, 1, 1);
    Expect(AoiLeaveGivenUpCount() == 0, "c not before cap");
    Expect(AoiLeavePendingCount() == 1, "c still pending before cap");
    MapRuntime::SetNowMsForTest(t0 + cap);
    AoiSetEvidenceNowMsForTest(t0 + cap);
    more.clear();
    rt.AdvancePresented(&more);
    Expect(AoiLeaveGivenUpCount() >= 1, "c given up at cap");
    const uint64_t gave = AoiLeaveGivenUpCount();
    Expect(AoiLeavePendingCount() == 0, "c pending cleared on give up");
    MapRuntime::SetNowMsForTest(t0 + cap + 120000);
    AoiSetEvidenceNowMsForTest(t0 + cap + 120000);
    more.clear();
    rt.AdvancePresented(&more);
    all = {};
    Collect(more, &all);
    Expect(AoiLeavePendingCount() == 0, "c pending stays clear");
    Expect(CountLeaveTo(all, 1, 1) == 0, "c no retry after give up");
    Expect(AoiLeaveGivenUpCount() == gave, "c give up once");
}

void TestRetryBackoff(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    AoiResetEvidenceForTest();
    rt.SetViewRadiusCells(1);
    rt.SetMaxVisible(50);
    const int64_t t0 = 2000000;
    MapRuntime::SetNowMsForTest(t0);
    AoiSetEvidenceNowMsForTest(t0);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(1, data, Ent(1, 5.f, 5.f), &self, &snap, &pushes, &err), "backoff enter viewer");
    Expect(rt.Enter(1, data, Ent(2, 6.f, 5.f), &self, &snap, &pushes, &err), "backoff enter subject");
    MapEntity conf;
    Expect(rt.Move(1, 2, 70.f, 0.f, 5.f, 0.f, 1, t0, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "backoff move");
    std::vector<AoiPushBatch> more;
    rt.AdvancePresented(&more);
    AoiPushBatch all;
    Collect(more, &all);
    Expect(FailLeaves(&rt, all, 1, 1) >= 1, "backoff first fail");
    int64_t now = t0;
    for (int i = 0; i < 6; ++i) {
        now += 30000;
        MapRuntime::SetNowMsForTest(now);
        AoiSetEvidenceNowMsForTest(now);
        more.clear();
        rt.AdvancePresented(&more);
        all = {};
        Collect(more, &all);
        FailLeaves(&rt, all, 1, 1);
    }
    now += 10000;
    MapRuntime::SetNowMsForTest(now);
    AoiSetEvidenceNowMsForTest(now);
    more.clear();
    rt.AdvancePresented(&more);
    all = {};
    Collect(more, &all);
    Expect(CountLeaveTo(all, 1, 1) == 0, "backoff holds past 10s after repeated fails");
    Expect(AoiLeavePendingCount() == 1, "backoff still pending");
}

int UnsentHigh(uint64_t id) { return id == 1 ? 20000 : -1; }

void TestUnsentSkipsRetry(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    AoiResetEvidenceForTest();
    rt.SetViewRadiusCells(1);
    rt.SetMaxVisible(50);
    const int64_t t0 = 3000000;
    MapRuntime::SetNowMsForTest(t0);
    AoiSetEvidenceNowMsForTest(t0);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Enter(1, data, Ent(1, 5.f, 5.f), &self, &snap, &pushes, &err), "unsent enter viewer");
    Expect(rt.Enter(1, data, Ent(2, 6.f, 5.f), &self, &snap, &pushes, &err), "unsent enter subject");
    MapEntity conf;
    Expect(rt.Move(1, 2, 70.f, 0.f, 5.f, 0.f, 1, t0, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "unsent move");
    std::vector<AoiPushBatch> more;
    rt.AdvancePresented(&more);
    AoiPushBatch all;
    Collect(more, &all);
    Expect(FailLeaves(&rt, all, 1, 1) >= 1, "unsent first fail");
    AoiSetViewerUnsentFnForTest(UnsentHigh);
    MapRuntime::SetNowMsForTest(t0 + 60000);
    AoiSetEvidenceNowMsForTest(t0 + 60000);
    more.clear();
    rt.AdvancePresented(&more);
    all = {};
    Collect(more, &all);
    Expect(CountLeaveTo(all, 1, 1) == 0, "unsent above 16KB does not retry");
    Expect(AoiLeavePendingCount() >= 1, "unsent keeps the pending leave");
    AoiSetViewerUnsentFnForTest(nullptr);
}

bool FailOneLeave(MapRuntime *rt, const std::shared_ptr<const MapStaticData> &data, int64_t now,
                  uint64_t *version) {
    rt->ClearForTest();
    AoiResetEvidenceForTest();
    rt->SetViewRadiusCells(1);
    rt->SetMaxVisible(50);
    MapRuntime::SetNowMsForTest(now);
    AoiSetEvidenceNowMsForTest(now);
    MapEntity self;
    std::vector<MapEntity> snap;
    AoiPushBatch pushes;
    std::string err;
    if (!rt->Enter(1, data, Ent(1, 5.f, 5.f), &self, &snap, &pushes, &err))
        return false;
    if (!rt->Enter(1, data, Ent(2, 6.f, 5.f), &self, &snap, &pushes, &err))
        return false;
    MapEntity conf;
    if (rt->Move(1, 2, 70.f, 0.f, 5.f, 0.f, 1, now, &conf, &pushes, &err, true) != MapMoveReject::Ok)
        return false;
    std::vector<AoiPushBatch> more;
    rt->AdvancePresented(&more);
    AoiPushBatch all;
    Collect(more, &all);
    uint64_t ver = 0;
    int noted = 0;
    for (const auto &ev : all.events) {
        if (ev.op != 3 || ev.recipient_id != 1 || ev.snapshot.player_id != 2 || ev.notify_version == 0)
            continue;
        rt->NoteAoiWrite(1, 2, 3, false, ev.notify_version);
        ver = ev.notify_version;
        ++noted;
    }
    if (version)
        *version = ver;
    return noted >= 1 && AoiLeavePendingForViewer(1) == 1;
}

int CountEnter(const AoiPushBatch &batch, uint64_t viewer, uint64_t subject) {
    int n = 0;
    for (const auto &ev : batch.events) {
        if (ev.op == 1 && ev.recipient_id == viewer && ev.snapshot.player_id == subject)
            ++n;
    }
    return n;
}

void TestBackInWindowClearsEvidence(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    const int64_t now = 4000000;
    Expect(FailOneLeave(&rt, data, now, nullptr), "window setup");
    Expect(rt.ViewerConnected(1), "window viewer online before return");
    const int before = AoiLeavePendingCount();
    MapEntity conf;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Move(1, 2, 6.f, 0.f, 5.f, 0.f, 2, now, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "window move back");
    std::vector<AoiPushBatch> more;
    rt.AdvancePresented(&more);
    AoiPushBatch all;
    Collect(more, &all);
    Expect(CountEnter(all, 1, 2) >= 1, "window sent ENTER");
    Expect(AoiLeavePendingForViewer(1) == 0, "window evidence cleared");
    Expect(AoiLeavePendingCount() == before - 1, "window pending decreased by one");
    Expect(rt.ViewerConnected(1), "window viewer stays online");
    Expect(AoiLeaveDropCount(AoiLeaveDropReason::BackInWindow) >= 1, "window counted back_in_window");
}

void TestVersionSupersededClearsEvidence(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    const int64_t now = 5000000;
    Expect(FailOneLeave(&rt, data, now, nullptr), "version setup");
    Expect(AoiLeavePendingForViewer(1) == 1, "version pending before supersede");
    MapEntity conf;
    AoiPushBatch pushes;
    std::string err;
    Expect(rt.Move(1, 1, 68.f, 0.f, 5.f, 0.f, 1, now, &conf, &pushes, &err, true) == MapMoveReject::Ok,
           "version viewer walks back");
    std::vector<AoiPushBatch> more;
    rt.AdvancePresented(&more);
    AoiPushBatch all;
    Collect(more, &all);
    Expect(CountEnter(all, 1, 2) >= 1, "version sent ENTER");
    Expect(AoiLeavePendingForViewer(1) == 0, "version evidence cleared");
    Expect(rt.ViewerConnected(1), "version viewer stays online");
    Expect(AoiLeaveDropCount(AoiLeaveDropReason::VersionSuperseded) >= 1, "version counted");
}

void TestLeaveAckClearsEvidence(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    uint64_t ver = 0;
    Expect(FailOneLeave(&rt, data, 6000000, &ver), "ack setup");
    Expect(ver != 0, "ack version");
    rt.NoteAoiWrite(1, 2, 3, true, ver);
    Expect(AoiLeavePendingForViewer(1) == 0, "ack evidence cleared");
    Expect(rt.ViewerConnected(1), "ack viewer stays online");
    Expect(AoiLeaveDropCount(AoiLeaveDropReason::Acked) >= 1, "ack counted");
}

void TestEnterAckClearsEvidence(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    uint64_t ver = 0;
    Expect(FailOneLeave(&rt, data, 7000000, &ver), "enter-ack setup");
    Expect(ver != 0, "enter-ack version");
    rt.NoteAoiWrite(1, 2, 1, true, ver);
    Expect(AoiLeavePendingForViewer(1) == 0, "enter ack clears failed leave");
    Expect(rt.ViewerConnected(1), "enter ack viewer stays online");
    Expect(AoiLeaveDropCount(AoiLeaveDropReason::BackInWindow) >= 1, "enter ack counted");
}

void TestInconsistentGauge(const std::shared_ptr<const MapStaticData> &data) {
    auto &rt = MapRuntime::Instance();
    rt.ClearForTest();
    AoiResetEvidenceForTest();
    AoiNoteLeaveWriteFail(42, 43, 1000);
    std::vector<AoiPushBatch> more;
    rt.AdvancePresented(&more);
    std::ostringstream os;
    AoiEvidenceWritePrometheus(os);
    const std::string text = os.str();
    Expect(text.find("gamemesh_aoi_leave_pending_inconsistent 1") != std::string::npos,
           "orphan evidence is inconsistent");
    uint64_t ver = 0;
    Expect(FailOneLeave(&rt, data, 8000000, &ver), "consistent setup");
    more.clear();
    rt.AdvancePresented(&more);
    os.str("");
    os.clear();
    AoiEvidenceWritePrometheus(os);
    Expect(os.str().find("gamemesh_aoi_leave_pending_inconsistent 0") != std::string::npos,
           "queued leave is consistent");
}

void TestOldestSplit() {
    AoiResetEvidenceForTest();
    AoiSetEvidenceNowMsForTest(5000);
    AoiSetViewerOnlineFn([](uint64_t, bool *online) {
        if (!online)
            return false;
        *online = false;
        return true;
    });
    AoiNoteLeaveWriteFail(9, 8, 1000);
    std::ostringstream os;
    AoiEvidenceWritePrometheus(os);
    const std::string text = os.str();
    Expect(text.find("gamemesh_aoi_leave_write_pending_oldest_ms{viewer=\"offline\"} 4000") != std::string::npos,
           "oldest offline split");
    Expect(text.find("gamemesh_aoi_leave_write_pending_oldest_ms{viewer=\"online\"}") == std::string::npos,
           "oldest online omitted when empty");
    AoiSetViewerOnlineFn(nullptr);
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::ERROR);
    auto data = MakeOpen();
    if (!data)
        return 1;
    TestDisconnectClearsPending(data);
    TestManyViewersDrop(data);
    TestGiveUpUsesFirstFail(data);
    TestRetryBackoff(data);
    TestUnsentSkipsRetry(data);
    TestBackInWindowClearsEvidence(data);
    TestVersionSupersededClearsEvidence(data);
    TestLeaveAckClearsEvidence(data);
    TestEnterAckClearsEvidence(data);
    TestInconsistentGauge(data);
    TestOldestSplit();
    MapRuntime::SetNowMsForTest(0);
    AoiSetEvidenceNowMsForTest(0);
    AoiSetViewerOnlineFn(nullptr);
    if (fails == 0)
        std::cout << "leave pending audit ok\n";
    return fails == 0 ? 0 : 1;
}
