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
#include <thread>

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

int64_t CountPending(uint64_t from, uint64_t to) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return -1;
    MYSQL_RES *res = conn->query("SELECT COUNT(*) FROM friend_request WHERE from_player_id=" +
                                 std::to_string(from) + " AND to_player_id=" + std::to_string(to) +
                                 " AND status=0");
    if (!res)
        return -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    const int64_t n = row && row[0] ? std::stoll(row[0]) : -1;
    mysql_free_result(res);
    return n;
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

    const uint64_t e = a + 4;
    const uint64_t f = a + 5;
    EnsurePlayer(e, "fe_" + std::to_string(e));
    EnsurePlayer(f, "ff_" + std::to_string(f));
    const std::string shared = "shared:" + std::to_string(suffix);
    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(e);
    req.set_target_player_id(f);
    req.set_idempotency_key(shared);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok(), "apply before key reuse");
    req.set_op("DELETE");
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_INVALID_ARGUMENT", "same key different op");
    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(e);
    req.set_target_player_id(d);
    req.set_idempotency_key(shared);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_INVALID_ARGUMENT", "same key different target");

    const uint64_t na = a + 6;
    const uint64_t nb = a + 7;
    const uint64_t nc = a + 8;
    const std::string name_a = "nA_" + std::to_string(suffix);
    const std::string name_b = "nB_" + std::to_string(suffix);
    EnsurePlayer(na, name_a);
    EnsurePlayer(nb, name_b);
    EnsurePlayer(nc, "nC_" + std::to_string(nc));

    req.Clear();
    req.set_op("SEARCH");
    req.set_actor_player_id(nc);
    req.set_target_player_id(na);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.player().player_id() == na && rsp.player().level() == 1, "search by id");

    req.Clear();
    req.set_op("SEARCH");
    req.set_actor_player_id(nc);
    req.set_exact_name(name_a);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.player().player_id() == na && rsp.player().name() == name_a,
           "search by exact name");

    req.Clear();
    req.set_op("SEARCH");
    req.set_actor_player_id(nc);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_INVALID_ARGUMENT", "search empty target");

    req.set_exact_name("missing_" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_PLAYER_NOT_FOUND", "search missing name");

    const uint64_t amb1 = a + 9;
    const uint64_t amb2 = a + 10;
    const std::string amb = "amb_" + std::to_string(suffix);
    EnsurePlayer(amb1, amb);
    EnsurePlayer(amb2, amb);
    req.Clear();
    req.set_op("SEARCH");
    req.set_actor_player_id(nc);
    req.set_exact_name(amb);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_NAME_AMBIGUOUS", "search ambiguous name");

    const std::string name_key = "name-apply:" + std::to_string(suffix);
    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(nc);
    req.set_exact_name(name_a);
    req.set_idempotency_key(name_key);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.request_id() != 0, "apply by name A");
    const uint64_t name_rid = rsp.request_id();

    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(nc);
    req.set_exact_name(name_b);
    req.set_idempotency_key(name_key);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_INVALID_ARGUMENT", "same key name A then name B");
    Expect(CountPending(nc, nb) == 0, "name B has no request");

    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(nc);
    req.set_target_player_id(na);
    req.set_idempotency_key(name_key);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.idempotent_hit() && rsp.request_id() == name_rid, "same key name A then id A");

    const std::string miss_key = "miss-name:" + std::to_string(suffix);
    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(nc);
    req.set_exact_name("missing_" + std::to_string(suffix));
    req.set_idempotency_key(miss_key);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_PLAYER_NOT_FOUND", "unresolved name stored");

    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(nc);
    req.set_target_player_id(nb);
    req.set_idempotency_key(miss_key);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_INVALID_ARGUMENT",
           "unresolved name key cannot switch to an id");
    Expect(CountPending(nc, nb) == 0, "switched id has no request");

    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(na);
    req.set_target_player_id(nb);
    req.set_idempotency_key(name_key);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok() && rsp.request_id() != 0, "different actor may reuse a key string");

    const uint64_t p1 = a + 11;
    const uint64_t p2 = a + 12;
    const uint64_t p3 = a + 13;
    EnsurePlayer(p1, "p1_" + std::to_string(p1));
    EnsurePlayer(p2, "p2_" + std::to_string(p2));
    EnsurePlayer(p3, "p3_" + std::to_string(p3));
    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(p1);
    req.set_target_player_id(p2);
    req.set_idempotency_key("acc-a:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok(), "apply for accept key");
    const uint64_t acc_rid = rsp.request_id();
    req.set_actor_player_id(p3);
    req.set_idempotency_key("acc-b:" + std::to_string(suffix));
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok(), "second apply for accept key");
    const uint64_t other_rid = rsp.request_id();
    const std::string acc_key = "accept-share:" + std::to_string(suffix);
    req.Clear();
    req.set_op("ACCEPT");
    req.set_actor_player_id(p2);
    req.set_request_id(acc_rid);
    req.set_idempotency_key(acc_key);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.ok(), "accept first request");
    req.set_request_id(other_rid);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(!rsp.ok() && rsp.error_code() == "ERR_INVALID_ARGUMENT", "same key different request_id");

    const uint64_t race_from = a + 14;
    const uint64_t race_to = a + 15;
    EnsurePlayer(race_from, "racef_" + std::to_string(race_from));
    EnsurePlayer(race_to, "racet_" + std::to_string(race_to));
    const std::string race_key = "race:" + std::to_string(suffix);
    gdb::FriendOpRsp race_a;
    gdb::FriendOpRsp race_b;
    std::thread t1([&]() {
        gdb::FriendOpReq one;
        one.set_op("APPLY");
        one.set_actor_player_id(race_from);
        one.set_target_player_id(race_to);
        one.set_idempotency_key(race_key);
        FriendStore::Instance().Execute(one, &race_a);
    });
    std::thread t2([&]() {
        gdb::FriendOpReq one;
        one.set_op("APPLY");
        one.set_actor_player_id(race_from);
        one.set_target_player_id(race_to);
        one.set_idempotency_key(race_key);
        FriendStore::Instance().Execute(one, &race_b);
    });
    t1.join();
    t2.join();
    const bool race_ok = race_a.ok() || race_b.ok();
    const bool race_safe =
        (!race_a.ok() || race_a.request_id() != 0) && (!race_b.ok() || race_b.request_id() != 0);
    Expect(race_ok && race_safe && CountPending(race_from, race_to) == 1, "concurrent same key");
    req.Clear();
    req.set_op("APPLY");
    req.set_actor_player_id(race_from);
    req.set_target_player_id(race_to);
    req.set_idempotency_key(race_key);
    FriendStore::Instance().Execute(req, &rsp);
    Expect(rsp.idempotent_hit() && rsp.ok() && CountPending(race_from, race_to) == 1,
           "concurrent key retries the stored apply");

    if (fails) {
        std::printf("friend_store_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK friend_store_test\n");
    return 0;
}
