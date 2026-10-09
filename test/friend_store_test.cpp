/**
 * 好友事务：双向关系、幂等 Accept、Block 清理申请。
 */
#include "Connection.h"
#include "ConnectionPool.h"
#include "FriendStore.h"
#include "Logging.h"
#include "PlayerProfileStore.h"
#include "gamedb.pb.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t EnsurePlayer(uint64_t pid, const std::string &name) {
    PlayerProfileRow row;
    PlayerProfileStore::FillDefaults(pid, name, &row);
    std::string err, code;
    PlayerProfileStore::Instance().EnsureDefault(pid, name, &err);
    return pid;
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        std::printf("FAIL: MySQL pool not initialized\n");
        return 1;
    }
    Expect(PlayerProfileStore::Instance().EnsureTable(), "profile table");

    const uint64_t suffix = static_cast<uint64_t>(NowMs());
    const uint64_t a = 800000000 + (suffix % 100000);
    const uint64_t b = a + 1;
    EnsurePlayer(a, "fa_" + std::to_string(a));
    EnsurePlayer(b, "fb_" + std::to_string(b));

    gdb::FriendOpReq req;
    gdb::FriendOpRsp rsp;
    req.set_op("APPLY");
    req.set_actor_player_id(a);
    req.set_target_player_id(b);
    req.set_idempotency_key("apply:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok(), "apply ok");
    Expect(rsp.request_id() != 0, "request_id");
    const uint64_t rid = rsp.request_id();

    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.idempotent_hit(), "apply idempotent");

    req.Clear();
    req.set_op("ACCEPT");
    req.set_actor_player_id(b);
    req.set_request_id(rid);
    req.set_idempotency_key("accept:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok(), "accept ok");

    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.idempotent_hit(), "accept retry");

    req.Clear();
    req.set_op("LIST");
    req.set_actor_player_id(a);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.friends_size() >= 1, "list a");

    req.set_actor_player_id(b);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.friends_size() >= 1, "list b");

    req.Clear();
    req.set_op("DELETE");
    req.set_actor_player_id(a);
    req.set_target_player_id(b);
    req.set_idempotency_key("del:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok(), "delete ok");

    req.Clear();
    req.set_op("LIST");
    req.set_actor_player_id(a);
    FriendStore::Instance().Execute(req, &rsp);
    bool still = false;
    for (int i = 0; i < rsp.friends_size(); ++i)
        if (rsp.friends(i).player_id() == b)
            still = true;
    Expect(!still, "deleted both sides from a");

    req.Clear();
    req.set_op("DELETE");
    req.set_actor_player_id(a);
    req.set_target_player_id(b);
    req.set_idempotency_key("del-missing:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_NOT_FRIEND", "delete missing friend");

    req.Clear();
    req.set_op("BLOCK");
    req.set_actor_player_id(a);
    req.set_target_player_id(b);
    req.set_idempotency_key("blk:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok(), "block ok");

    req.Clear();
    req.set_op("BLOCK_GATE");
    req.set_actor_player_id(a);
    req.set_target_player_id(b);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_ALREADY_BLOCKED", "gate self block");

    req.set_actor_player_id(b);
    req.set_target_player_id(a);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.privacy_ok(), "gate hidden when blocked by peer");

    req.Clear();
    req.set_op("DELETE");
    req.set_actor_player_id(a);
    req.set_target_player_id(b);
    req.set_idempotency_key("del-missing:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_NOT_FRIEND" && rsp.idempotent_hit(),
           "delete missing retries the stored error");

    const uint64_t c = a + 2;
    const uint64_t d = a + 3;
    EnsurePlayer(c, "fc_" + std::to_string(c));
    EnsurePlayer(d, "fd_" + std::to_string(d));
    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(c);
    req.set_target_player_id(d);
    req.set_idempotency_key("apply-exp:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.request_id() != 0, "apply for expiry");
    const uint64_t exp_rid = rsp.request_id();
    {
        auto conn = ConnectionPool::getconnectionPool()->getConnection();
        Expect(conn != nullptr, "mysql for expiry update");
        if (conn) {
            Expect(conn->update("UPDATE friend_request SET expire_at=1 WHERE request_id=" +
                                std::to_string(exp_rid)),
                   "force expire");
        }
    }
    req.Clear();
    req.set_op("ACCEPT");
    req.set_actor_player_id(d);
    req.set_request_id(exp_rid);
    req.set_idempotency_key("accept-exp:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_REQUEST_EXPIRED", "accept expired");

    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(c);
    req.set_target_player_id(d);
    req.set_idempotency_key("apply-again:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.request_id() != 0 && rsp.request_id() != exp_rid,
           "expired pending does not block a new apply");

    Expect(FriendStore::Instance().ExpireStaleRequests(50) >= 0, "expire sweep runs");

    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(c);
    req.set_target_player_id(d);
    req.set_idempotency_key(std::string(97, 'k'));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_INVALID_ARGUMENT", "idempotency key length");

    if (fails) {
        std::printf("friend_store_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK friend_store_test\n");
    return 0;
}
