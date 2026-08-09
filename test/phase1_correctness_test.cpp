/**
 * 阶段一 §八 专项：串行队列、client_seq、可信身份、lease、Formal 禁本地 Claim、Channel 快照
 */
#include "ClientSeqGate.h"
#include "FormalMode.h"
#include "GameLogic.h"
#include "GatewayConnRegistry.h"
#include "MapInstanceRegistry.h"
#include "MapPlacement.h"
#include "PlayerSerialQueue.h"
#include "TrustedPlayerId.h"
#include "game.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
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
    Expect(EvaluateClientSeq(1, 3, "", &cached, &err) == ClientSeqDecision::Reject, "seq ooo");
    Expect(EvaluateClientSeq(1, 2, "", &cached, &err) == ClientSeqDecision::Execute, "seq next");
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
    Expect(rsp.enter_map().message().find("placement") != std::string::npos ||
               rsp.message().find("placement") != std::string::npos,
           "formal message placement unavailable");
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

}  // namespace

int main() {
    TestSerialQueue100AndParallel();
    TestClientSeq();
    TestTrustedPlayerId();
    TestLeaseAndFormalNoLocalClaim();
    TestStickyForget();
    if (g_fail) {
        std::printf("FAIL phase1_correctness_test fails=%d\n", g_fail);
        return 1;
    }
    std::printf("PASS phase1_correctness_test\n");
    return 0;
}
