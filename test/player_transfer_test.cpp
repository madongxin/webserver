/**
 * Remediation 阶段 2：Session Transfer Lua + GatewayConnRegistry ApplyRoute
 */
#include "GatewayConnRegistry.h"
#include "RedisPool.h"
#include "ServerStats.h"
#include "SessionStore.h"

#include <iostream>
#include <string>

namespace {

int g_fail = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::cerr << "FAIL: " << m << "\n";
        ++g_fail;
    } else {
        std::cout << "ok: " << m << "\n";
    }
}

bool EnsureSession(uint64_t pid, std::string *fence, uint64_t *rv, std::string *from_logic) {
    if (!SessionStore::Instance().InitFromConfig() || !SessionStore::Instance().Available())
        return false;
    SessionStore::Instance().SetLogicInstanceIds({"gl-0", "gl-1"});
    game::LoginReq req;
    req.set_player_id(pid);
    req.set_device_id("xfer-dev");
    req.set_server_id(1);
    game::LoginRsp rsp;
    if (!SessionStore::Instance().Login(req, &rsp) || !rsp.ok())
        return false;
    *fence = rsp.token();
    SessionRecord rec;
    std::string st, tid, err;
    if (!SessionStore::Instance().GetPlayerRoute(pid, *fence, &rec, &st, &tid, &err))
        return false;
    *rv = rec.route_version;
    *from_logic = rec.gamelogic_instance_id.empty() ? "gl-0" : rec.gamelogic_instance_id;
    return true;
}

void TestTransferSm() {
    const uint64_t pid = 910001;
    std::string fence;
    uint64_t rv = 0;
    std::string from_logic;
    if (!EnsureSession(pid, &fence, &rv, &from_logic)) {
        std::cerr << "FAIL player_transfer_test (Redis unavailable)\n";
        Expect(false, "redis session required");
        return;
    }
    Expect(true, "session ready");
    const std::string to_logic = (from_logic == "gl-0") ? "gl-1" : "gl-0";

    SessionStore::TransferBeginIn bin;
    bin.player_id = pid;
    bin.fence_token = fence;
    bin.expected_route_version = rv;
    bin.from_logic = from_logic;
    bin.to_logic = to_logic;
    bin.map_instance_id = 42;
    bin.map_owner_epoch = 7;
    SessionStore::TransferBeginOut bout;
    Expect(SessionStore::Instance().BeginPlayerTransfer(bin, &bout) && bout.ok, "BeginTransfer");
    Expect(!bout.transfer_id.empty(), "transfer_id issued");
    Expect(bout.route_state == "TRANSFERRING", "state TRANSFERRING");

    // 幂等 Begin
    bin.transfer_id = bout.transfer_id;
    SessionStore::TransferBeginOut bout2;
    Expect(SessionStore::Instance().BeginPlayerTransfer(bin, &bout2) && bout2.ok,
           "BeginTransfer idempotent");

    // Prepare 失败场景：Abort 后可恢复
    std::string aerr;
    uint64_t arv = 0;
    Expect(SessionStore::Instance().AbortPlayerTransfer(pid, fence, bout.transfer_id, &aerr, &arv),
           "AbortTransfer");

    // 重新 Begin + Commit
    bin.transfer_id.clear();
    Expect(SessionStore::Instance().BeginPlayerTransfer(bin, &bout) && bout.ok, "Begin again");
    SessionStore::TransferCommitIn cin;
    cin.player_id = pid;
    cin.fence_token = fence;
    cin.transfer_id = bout.transfer_id;
    cin.to_logic = to_logic;
    cin.map_instance_id = 42;
    cin.map_owner_epoch = 7;
    SessionStore::TransferCommitOut cout;
    Expect(SessionStore::Instance().CommitPlayerTransfer(cin, &cout) && cout.ok, "CommitTransfer");
    Expect(cout.gamelogic_instance_id == to_logic, "committed logic switched");
    Expect(cout.route_version > rv, "route_version bumped");
    Expect(cout.route_state == "ONLINE", "state ONLINE after commit");

    // Commit 幂等
    Expect(SessionStore::Instance().CommitPlayerTransfer(cin, &cout) && cout.ok,
           "Commit idempotent");

    SessionRecord rec;
    std::string st, tid, err;
    Expect(SessionStore::Instance().GetPlayerRoute(pid, fence, &rec, &st, &tid, &err),
           "GetPlayerRoute");
    Expect(rec.gamelogic_instance_id == to_logic, "authoritative route switched");
    Expect(st == "ONLINE" || st.empty(), "route_state online");
}

void TestApplyRoute() {
    auto &reg = GatewayConnRegistry::Instance();
    reg.Forget(5);
    GatewayConnRegistry::Bind b;
    b.player_id = 1;
    b.session_id = "s";
    b.gamelogic_instance_id = "gl-0";
    b.route_version = 3;
    reg.Remember(5, b);
    Expect(reg.ApplyRoute(5, "gl-1", 9, 2, 4), "ApplyRoute newer rv");
    GatewayConnRegistry::Bind out;
    Expect(reg.FindByConnection(5, &out) && out.gamelogic_instance_id == "gl-1", "sticky gl-1");
    const uint64_t rejected_before =
        ServerStats::route_apply_rejected.load(std::memory_order_relaxed);
    Expect(!reg.ApplyRoute(5, "gl-0", 1, 1, 2), "reject stale rv");
    Expect(ServerStats::route_apply_rejected.load(std::memory_order_relaxed) ==
               rejected_before + 1,
           "route_apply_rejected_count");
    Expect(reg.FindByConnection(5, &out) && out.gamelogic_instance_id == "gl-1",
           "sticky unchanged on stale");
    reg.Forget(5);
}

}  // namespace

void TestTransferTimeout() {
    const uint64_t pid = 910088;
    std::string fence;
    uint64_t rv = 0;
    std::string from_logic;
    if (!EnsureSession(pid, &fence, &rv, &from_logic)) {
        Expect(false, "redis session required for transfer ttl");
        return;
    }
    const std::string to_logic = (from_logic == "gl-0") ? "gl-1" : "gl-0";
    SessionStore::TransferBeginIn bin;
    bin.player_id = pid;
    bin.fence_token = fence;
    bin.expected_route_version = rv;
    bin.from_logic = from_logic;
    bin.to_logic = to_logic;
    bin.map_instance_id = 42;
    bin.map_owner_epoch = 7;
    bin.transfer_id = "ttl-xfer";
    SessionStore::TransferBeginOut bout;
    Expect(SessionStore::Instance().BeginPlayerTransfer(bin, &bout) && bout.ok, "Begin for ttl");
    auto lease = RedisPool::Instance().Acquire();
    Expect(static_cast<bool>(lease), "redis lease");
    if (!lease)
        return;
    const std::string skey = SessionStore::Instance().key_prefix() + "session:" + std::to_string(pid);
    const std::string zkey = SessionStore::Instance().key_prefix() + "transfer:deadlines";
    std::vector<std::string> reply;
    Expect(lease->Eval("redis.call('HSET', KEYS[1], 'transferDeadline', '1') "
                       "redis.call('ZADD', KEYS[2], 1, ARGV[1]) return {'1'}",
                       {skey, zkey}, {std::to_string(pid)}, &reply),
           "force transfer deadline");
    lease = RedisPool::Lease();
    Expect(SessionStore::Instance().ExpireDueTransfers(100) >= 1, "transfer ttl abort");
    SessionRecord rec;
    std::string st, tid, err;
    Expect(SessionStore::Instance().GetPlayerRoute(pid, fence, &rec, &st, &tid, &err),
           "route after abort");
    Expect(st != "TRANSFERRING", "transfer rolled back");
    game::LogoutReq lo;
    lo.set_player_id(pid);
    game::LogoutRsp lor;
    SessionStore::Instance().Logout(lo, &lor);
}

int main() {
    TestApplyRoute();
    TestTransferSm();
    TestTransferTimeout();
    if (g_fail) {
        std::cerr << "player_transfer_test FAIL count=" << g_fail << "\n";
        return 1;
    }
    std::cout << "player_transfer_test PASS\n";
    return 0;
}
