/**
 * 阶段一 §八 专项：串行队列、client_seq、可信身份、lease、Formal 禁本地 Claim、Channel 快照
 */
#include "ClientSeqGate.h"
#include "FormalMode.h"
#include "GameLogic.h"
#include "GatewayConnRegistry.h"
#include "MapCatalog.h"
#include "MapInstanceRegistry.h"
#include "MapPlacement.h"
#include "MapRuntime.h"
#include "MapStaticData.h"
#include "PlayerSerialQueue.h"
#include "TrustedPlayerId.h"
#include "game.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;

void Expect(bool c, const char *msg) {
    if (!c) {
        std::printf("FAIL %s\n", msg);
        ++g_fail;
    } else {
        std::printf("ok %s\n", msg);
    }
}

void TestSerialQueue100AndParallel() {
    auto &q = PlayerSerialQueue::Instance();
    q.Stop();
    q.SetLimits(256, 4096);
    q.Start(4);

    constexpr int kN = 100;
    std::vector<int> order;
    order.reserve(kN);
    std::mutex mu;
    bool posted = true;
    for (int i = 0; i < kN; ++i) {
        posted = q.TryPost(7,
                           [i, &order, &mu]() {
                               std::lock_guard<std::mutex> lk(mu);
                               order.push_back(i);
                           }) &&
                 posted;
    }
    Expect(posted, "TryPost 100");
    q.DrainForTest();
    Expect(static_cast<int>(order.size()) == kN, "100 cmds size");
    bool ordered = true;
    for (int i = 0; i < kN; ++i) {
        if (order[static_cast<size_t>(i)] != i)
            ordered = false;
    }
    Expect(ordered, "same player 100 cmds ordered");

    std::atomic<int> a{0}, b{0};
    std::atomic<bool> a_started{false}, b_started{false};
    std::mutex gate_mu;
    std::condition_variable gate_cv;
    bool release = false;
    Expect(q.TryPost(1,
                     [&]() {
                         a_started.store(true);
                         std::unique_lock<std::mutex> lk(gate_mu);
                         gate_cv.wait(lk, [&]() { return release; });
                         a.fetch_add(1);
                     }),
           "post player1");
    Expect(q.TryPost(2,
                     [&]() {
                         b_started.store(true);
                         std::unique_lock<std::mutex> lk(gate_mu);
                         gate_cv.wait(lk, [&]() { return release; });
                         b.fetch_add(1);
                     }),
           "post player2");
    for (int i = 0; i < 200 && !(a_started.load() && b_started.load()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Expect(a_started.load() && b_started.load(), "two players progress in parallel");
    {
        std::lock_guard<std::mutex> lk(gate_mu);
        release = true;
    }
    gate_cv.notify_all();
    q.DrainForTest();
    Expect(a.load() == 1 && b.load() == 1, "two players completed");
    q.Stop();
}

void TestClientSeq() {
    std::string cached, err;
    Expect(EvaluateClientSeq(0, 1, "", &cached, &err) == ClientSeqDecision::Execute, "seq first");
    Expect(EvaluateClientSeq(1, 1, "frame-1", &cached, &err) == ClientSeqDecision::Idempotent &&
               cached == "frame-1",
           "seq idempotent");
    Expect(EvaluateClientSeq(1, 3, "", &cached, &err) == ClientSeqDecision::Execute,
           "seq gap after gateway-local heartbeat");
    Expect(EvaluateClientSeq(4, 6, "", &cached, &err) == ClientSeqDecision::Execute,
           "enter=4 heartbeat=5 move=6");
    Expect(EvaluateClientSeq(1, 2, "", &cached, &err) == ClientSeqDecision::Execute, "seq next");
    Expect(EvaluateClientSeq(6, 4, "", &cached, &err) == ClientSeqDecision::Reject, "seq stale rewind");
    Expect(EvaluateClientSeq(5, 0, "", &cached, &err) == ClientSeqDecision::Execute,
           "seq 0 skip check");
}

void TestTrustedPlayerId() {
    game::GameRequest consume;
    consume.mutable_consume_item()->set_player_id(999);
    ApplyTrustedPlayerId(&consume, 42);
    Expect(consume.consume_item().player_id() == 42, "trusted overwrite consume");
    game::GameRequest enter;
    enter.mutable_enter_map()->set_player_id(999);
    ApplyTrustedPlayerId(&enter, 42);
    Expect(enter.enter_map().player_id() == 42, "trusted overwrite enter_map");
}

void TestLeaseAndFormalNoLocalClaim() {
    auto &reg = MapInstanceRegistry::Instance();
    reg.ClearForTest();
    reg.SetRequireLease(false);
    Expect(reg.Claim(200, 1, 1, 1), "claim past lease");
    Expect(reg.CheckWrite(200, 1) == MapWriteFence::LeaseExpired, "lease expired");
    Expect(reg.Claim(201, 1, 2, 0), "claim lease0");
    reg.SetRequireLease(true);
    Expect(reg.CheckWrite(201, 2) == MapWriteFence::LeaseMissing, "lease missing formal");
    Expect(reg.CheckWrite(201, 9) == MapWriteFence::StaleEpoch, "stale epoch");

    ::setenv("GAMEMESH_FORMAL", "1", 1);
    Expect(FormalModeEnabled(), "formal on");
    reg.SetLocalInstanceId("gl-test");
    MapPlacement::Instance().ConfigureOwners({"gl-test"});
    game::GameRequest req;
    req.mutable_enter_map()->set_player_id(1);
    req.mutable_enter_map()->set_realm_id(1);
    req.mutable_enter_map()->set_map_template_id(1001);
    game::GameResponse rsp;
    Expect(!GameLogic::Instance().Handle(req, &rsp), "formal reject local claim");
    {
        const std::string &m =
            !rsp.enter_map().message().empty() ? rsp.enter_map().message() : rsp.message();
        Expect(m.find("ERR_PLACEMENT_") != std::string::npos ||
                   m.find("placement") != std::string::npos,
               "formal message placement required/unavailable");
    }
    ::unsetenv("GAMEMESH_FORMAL");
    reg.SetRequireLease(false);
    reg.ClearForTest();
}

void TestStickyForget() {
    auto &reg = GatewayConnRegistry::Instance();
    reg.Forget(1);
    reg.Forget(2);
    GatewayConnRegistry::Bind oldb;
    oldb.player_id = 55;
    oldb.session_id = "s55";
    oldb.generation = 1;
    oldb.gateway_instance_id = "gw-0";
    reg.Remember(1, oldb);
    GatewayConnRegistry::Bind newb = oldb;
    newb.generation = 2;
    newb.gateway_instance_id = "gw-1";
    reg.Remember(2, newb);
    reg.Forget(1);
    GatewayConnRegistry::Bind found;
    Expect(reg.FindByPlayer(55, &found) && found.connection_id == 2, "old Forget keeps new index");
    reg.Forget(2);
}

void TestEnterMapHashAndMove() {
    auto &reg = MapInstanceRegistry::Instance();
    reg.ClearForTest();
    reg.SetRequireLease(false);
    reg.SetLocalInstanceId("gl-test");
    MapPlacement::Instance().ConfigureOwners({"gl-test"});
    MapRuntime::Instance().ClearForTest();
    MapCatalog::Instance().ClearForTest();

    const std::string json = R"JSON({
      "schema_version": 1,
      "map_template_id": 9001,
      "scene_name": "Tiny",
      "data_version": 1,
      "bounds_min": [0, 0, 0],
      "bounds_max": [20, 5, 20],
      "aoi_cell_size": 10.0,
      "nav_sample_step": 1.0,
      "grid_width": 20,
      "grid_height": 20,
      "walkable_rle": [1, 400],
      "spawn_points": [{"id":"default","position":[1, 0, 1],"yaw":0}]
    })JSON";
    std::shared_ptr<const MapStaticData> data;
    std::string err;
    Expect(MapStaticData::LoadFromJson(json, &data, &err), "s2 tiny map json");
    if (!data)
        return;
    MapCatalog::Instance().PutForTest(data);

    game::GameRequest enter;
    enter.set_seq(1);
    auto *e = enter.mutable_enter_map();
    e->set_player_id(42);
    e->set_realm_id(1);
    e->set_map_template_id(9001);
    game::GameResponse rsp;
    Expect(GameLogic::Instance().Handle(enter, &rsp) && rsp.ok() && rsp.enter_map().ok(),
           "enter map 9001");
    Expect(rsp.enter_map().map_data_version() == 1, "enter returns data_version");
    Expect(rsp.enter_map().map_data_sha256() == data->sha256(), "enter returns sha256");
    Expect(rsp.enter_map().has_spawn_position(), "enter spawn");
    Expect(rsp.enter_map().has_self() && rsp.enter_map().self().player_id() == 42, "enter self");

    game::GameRequest bad;
    bad.set_seq(2);
    auto *b = bad.mutable_enter_map();
    b->set_player_id(43);
    b->set_realm_id(1);
    b->set_map_template_id(9001);
    b->set_map_data_version(1);
    b->set_map_data_sha256(std::string(64, '0'));
    game::GameResponse brsp;
    Expect(!GameLogic::Instance().Handle(bad, &brsp), "mismatch enter rejected");
    Expect(brsp.enter_map().message().find("ERR_MAP_DATA_MISMATCH") != std::string::npos,
           "mismatch code");
    Expect(brsp.enter_map().map_data_sha256() == data->sha256(), "mismatch returns server hash");

    game::GameRequest mv;
    mv.set_seq(3);
    auto *m = mv.mutable_move();
    m->set_player_id(42);
    m->set_map_instance_id(rsp.enter_map().map_instance_id());
    m->mutable_position()->set_x(2);
    m->mutable_position()->set_y(0);
    m->mutable_position()->set_z(1);
    game::GameResponse mrsp;
    Expect(GameLogic::Instance().Handle(mv, &mrsp) && mrsp.ok() && mrsp.move().ok(), "move ok");
    Expect(mrsp.move().state_seq() > 0, "move state_seq");

    mv.set_seq(4);
    m->mutable_position()->set_x(std::numeric_limits<float>::quiet_NaN());
    game::GameResponse nrsp;
    Expect(!GameLogic::Instance().Handle(mv, &nrsp), "nan move rejected");
    Expect(nrsp.move().error_code() == "ERR_INVALID_POSITION", "nan error_code");

    MapRuntime::Instance().ClearForTest();
    MapCatalog::Instance().ClearForTest();
    reg.ClearForTest();
}

}  // namespace

int main() {
    TestSerialQueue100AndParallel();
    TestClientSeq();
    TestTrustedPlayerId();
    TestLeaseAndFormalNoLocalClaim();
    TestEnterMapHashAndMove();
    TestStickyForget();
    if (g_fail) {
        std::printf("FAIL phase1_correctness_test fails=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS phase1_correctness_test\n");
    return 0;
}
