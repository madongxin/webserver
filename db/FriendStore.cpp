#include "FriendStore.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "Logging.h"
#include <mysql/mysql.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

enum RequestStatus : int {
    kPending = 0,
    kAccepted = 1,
    kRejected = 2,
    kCanceled = 3,
    kExpired = 4,
};

int64_t NowUnix() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

int EnvInt(const char *k, int def, int lo, int hi) {
    const char *e = std::getenv(k);
    if (!e || !*e)
        return def;
    char *end = nullptr;
    const long v = std::strtol(e, &end, 10);
    if (end == e)
        return def;
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return static_cast<int>(v);
}

int FriendCap() { return EnvInt("GAMEMESH_FRIEND_MAX_COUNT", 100, 1, 500); }
int BlockCap() { return EnvInt("GAMEMESH_FRIEND_BLOCK_MAX_COUNT", 100, 1, 500); }
int PendingCap() { return EnvInt("GAMEMESH_FRIEND_REQUEST_MAX_PENDING", 50, 1, 200); }
int ExpireDays() { return EnvInt("GAMEMESH_FRIEND_REQUEST_EXPIRE_DAYS", 7, 1, 90); }

void Fail(gdb::FriendOpRsp *rsp, const char *code, const char *msg) {
    if (!rsp)
        return;
    rsp->set_ok(false);
    rsp->set_error_code(code);
    rsp->set_message(msg);
}

void Ok(gdb::FriendOpRsp *rsp) {
    rsp->set_ok(true);
    rsp->set_error_code("OK");
    rsp->set_message("ok");
}

uint64_t ParseU64(const char *s) {
    if (!s)
        return 0;
    return static_cast<uint64_t>(std::strtoull(s, nullptr, 10));
}

int64_t CountSql(Connection *conn, const std::string &sql) {
    MYSQL_RES *res = conn->query(sql);
    if (!res)
        return -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    const int64_t n = row && row[0] ? static_cast<int64_t>(std::strtoll(row[0], nullptr, 10)) : 0;
    mysql_free_result(res);
    return n;
}

bool LoadProfile(Connection *conn, uint64_t pid, gdb::FriendBriefDb *out) {
    if (!out || pid == 0)
        return false;
    std::ostringstream os;
    os << "SELECT player_id,player_name FROM player_profile WHERE player_id=" << pid << " LIMIT 1";
    MYSQL_RES *res = conn->query(os.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    const bool ok = row && row[0];
    if (ok) {
        out->set_player_id(ParseU64(row[0]));
        out->set_name(row[1] ? row[1] : "");
        out->set_level(1);
    }
    mysql_free_result(res);
    return ok;
}

bool LoadProfileByName(Connection *conn, const std::string &name, gdb::FriendBriefDb *out,
                       bool *ambiguous) {
    if (ambiguous)
        *ambiguous = false;
    if (!out || name.empty())
        return false;
    const std::string lit = conn->EscapeSql(name);
    MYSQL_RES *res =
        conn->query("SELECT player_id,player_name FROM player_profile WHERE player_name='" + lit +
                    "' LIMIT 3");
    if (!res)
        return false;
    std::vector<MYSQL_ROW> rows;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
        rows.push_back(row);
    if (rows.size() >= 2) {
        if (ambiguous)
            *ambiguous = true;
        mysql_free_result(res);
        return false;
    }
    const bool ok = rows.size() == 1 && rows[0] && rows[0][0];
    if (ok) {
        out->set_player_id(ParseU64(rows[0][0]));
        out->set_name(rows[0][1] ? rows[0][1] : "");
        out->set_level(1);
    }
    mysql_free_result(res);
    return ok;
}

bool Blocked(Connection *conn, uint64_t a, uint64_t b) {
    std::ostringstream os;
    os << "SELECT 1 FROM friend_block WHERE player_id=" << a << " AND blocked_player_id=" << b
       << " LIMIT 1";
    MYSQL_RES *res = conn->query(os.str());
    if (!res)
        return false;
    const bool y = mysql_fetch_row(res) != nullptr;
    mysql_free_result(res);
    return y;
}

bool IsFriend(Connection *conn, uint64_t a, uint64_t b) {
    std::ostringstream os;
    os << "SELECT 1 FROM friend_relation WHERE player_id=" << a << " AND friend_player_id=" << b
       << " LIMIT 1";
    MYSQL_RES *res = conn->query(os.str());
    if (!res)
        return false;
    const bool y = mysql_fetch_row(res) != nullptr;
    mysql_free_result(res);
    return y;
}

int64_t PendingId(Connection *conn, uint64_t from, uint64_t to, int64_t now, bool lock) {
    std::ostringstream os;
    os << "SELECT request_id FROM friend_request WHERE from_player_id=" << from
       << " AND to_player_id=" << to << " AND status=" << kPending << " AND expire_at>=" << now
       << " LIMIT 1";
    if (lock)
        os << " FOR UPDATE";
    MYSQL_RES *res = conn->query(os.str());
    if (!res)
        return -1;
    MYSQL_ROW row = mysql_fetch_row(res);
    const int64_t id = row && row[0] ? static_cast<int64_t>(ParseU64(row[0])) : 0;
    mysql_free_result(res);
    return id;
}

bool LoadIdempotency(Connection *conn, uint64_t actor, const std::string &key,
                     gdb::FriendOpRsp *rsp) {
    if (!conn || key.empty() || actor == 0)
        return false;
    const std::string lit = conn->EscapeSql(key);
    std::ostringstream os;
    os << "SELECT error_code,request_id,peer_player_id FROM friend_op_idempotency WHERE "
          "actor_player_id="
       << actor << " AND idempotency_key='" << lit << "' LIMIT 1";
    MYSQL_RES *res = conn->query(os.str());
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return false;
    }
    rsp->set_idempotent_hit(true);
    const std::string code = row[0] ? row[0] : "OK";
    rsp->set_error_code(code);
    rsp->set_ok(code == "OK");
    rsp->set_message(code == "OK" ? "ok" : code);
    rsp->set_request_id(ParseU64(row[1]));
    rsp->set_peer_player_id(ParseU64(row[2]));
    mysql_free_result(res);
    return true;
}

bool SaveIdempotency(Connection *conn, uint64_t actor, const std::string &key, const std::string &op,
                     const gdb::FriendOpRsp &rsp) {
    if (!conn || key.empty() || actor == 0)
        return true;
    const std::string k = conn->EscapeSql(key);
    const std::string o = conn->EscapeSql(op);
    const std::string c = conn->EscapeSql(rsp.error_code().empty() ? "OK" : rsp.error_code());
    std::ostringstream os;
    os << "INSERT IGNORE INTO friend_op_idempotency(actor_player_id,idempotency_key,op,error_code,"
          "request_id,peer_player_id,created_at) VALUES("
       << actor << ",'" << k << "','" << o << "','" << c << "'," << rsp.request_id() << ","
       << rsp.peer_player_id() << "," << NowUnix() << ")";
    return conn->update(os.str());
}

uint32_t PageSize(const gdb::FriendOpReq &req, uint32_t defv, uint32_t cap) {
    uint32_t n = req.page_size() == 0 ? defv : req.page_size();
    if (n > cap)
        n = cap;
    if (n == 0)
        n = defv;
    return n;
}

uint64_t CursorId(const std::string &c) {
    if (c.empty())
        return 0;
    return ParseU64(c.c_str());
}

void FillBrief(gdb::FriendBriefDb *dst, uint64_t pid, const std::string &name,
               const std::string &remark) {
    dst->set_player_id(pid);
    dst->set_name(name);
    dst->set_level(1);
    dst->set_remark(remark);
}

}  // namespace

FriendStore &FriendStore::Instance() {
    static FriendStore g;
    return g;
}

bool FriendStore::EnsureTables() {
    auto *pool = ConnectionPool::getconnectionPool();
    if (!pool || !pool->isInitialized())
        return false;
    auto conn = pool->getConnection();
    if (!conn)
        return false;
    (void)conn;
    return true;
}

void FriendStore::Execute(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    if (!rsp)
        return;
    rsp->Clear();
    if (req.actor_player_id() == 0) {
        Fail(rsp, "ERR_UNAUTHENTICATED", "actor required");
        return;
    }
    auto *pool = ConnectionPool::getconnectionPool();
    if (!pool || !pool->isInitialized()) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    const std::string &op = req.op();
    if (op == "LIST")
        ListFriends(req, rsp);
    else if (op == "SEARCH")
        Search(req, rsp);
    else if (op == "APPLY")
        Apply(req, rsp);
    else if (op == "REQUEST_LIST")
        RequestList(req, rsp);
    else if (op == "ACCEPT")
        Accept(req, rsp);
    else if (op == "REJECT")
        Reject(req, rsp);
    else if (op == "DELETE")
        DeleteFriend(req, rsp);
    else if (op == "BLOCK")
        Block(req, rsp);
    else if (op == "UNBLOCK")
        Unblock(req, rsp);
    else if (op == "BLOCK_LIST")
        BlockList(req, rsp);
    else
        Fail(rsp, "ERR_INVALID_ARGUMENT", "unknown friend op");
}

void FriendStore::ListFriends(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    const uint64_t after = CursorId(req.cursor());
    const uint32_t lim = PageSize(req, 100, 100);
    std::ostringstream os;
    os << "SELECT r.friend_player_id,IFNULL(p.player_name,''),r.remark FROM friend_relation r "
          "LEFT JOIN player_profile p ON p.player_id=r.friend_player_id WHERE r.player_id="
       << req.actor_player_id();
    if (after != 0)
        os << " AND r.friend_player_id>" << after;
    os << " ORDER BY r.friend_player_id ASC LIMIT " << (lim + 1);
    MYSQL_RES *res = conn->query(os.str());
    if (!res) {
        Fail(rsp, "ERR_INTERNAL", "list query failed");
        return;
    }
    std::vector<std::tuple<uint64_t, std::string, std::string>> rows;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        rows.emplace_back(ParseU64(row[0]), row[1] ? row[1] : "", row[2] ? row[2] : "");
    }
    mysql_free_result(res);
    const int64_t total = CountSql(conn.get(), "SELECT COUNT(*) FROM friend_relation WHERE player_id=" +
                                                   std::to_string(req.actor_player_id()));
    bool more = rows.size() > lim;
    if (more)
        rows.resize(lim);
    for (const auto &t : rows) {
        FillBrief(rsp->add_friends(), std::get<0>(t), std::get<1>(t), std::get<2>(t));
    }
    if (more && !rows.empty())
        rsp->set_next_cursor(std::to_string(std::get<0>(rows.back())));
    rsp->set_friend_n(static_cast<uint32_t>(total < 0 ? rows.size() : total));
    rsp->set_friend_cap(static_cast<uint32_t>(FriendCap()));
    Ok(rsp);
}

void FriendStore::Search(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    gdb::FriendBriefDb brief;
    if (req.target_player_id() != 0) {
        if (!LoadProfile(conn.get(), req.target_player_id(), &brief)) {
            Fail(rsp, "ERR_PLAYER_NOT_FOUND", "not found");
            return;
        }
    } else if (!req.exact_name().empty()) {
        bool amb = false;
        if (!LoadProfileByName(conn.get(), req.exact_name(), &brief, &amb)) {
            Fail(rsp, amb ? "ERR_NAME_AMBIGUOUS" : "ERR_PLAYER_NOT_FOUND",
                 amb ? "ambiguous" : "not found");
            return;
        }
    } else {
        Fail(rsp, "ERR_INVALID_ARGUMENT", "target required");
        return;
    }
    const uint64_t a = req.actor_player_id();
    const uint64_t b = brief.player_id();
    int rel = 0;
    if (Blocked(conn.get(), a, b))
        rel = 4;
    else if (IsFriend(conn.get(), a, b))
        rel = 1;
    else {
        const int64_t now = NowUnix();
        const int64_t out = PendingId(conn.get(), a, b, now, false);
        const int64_t in = PendingId(conn.get(), b, a, now, false);
        if (out > 0)
            rel = 2;
        else if (in > 0)
            rel = 3;
    }
    *rsp->mutable_player() = brief;
    rsp->set_relation(rel);
    rsp->set_peer_player_id(b);
    Ok(rsp);
}

void FriendStore::Apply(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    if (!req.idempotency_key().empty() &&
        LoadIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), rsp))
        return;
    if (!conn->begin()) {
        Fail(rsp, "ERR_INTERNAL", "begin failed");
        return;
    }
    auto rollback = [&]() { conn->rollback(); };
    gdb::FriendBriefDb target;
    uint64_t b = req.target_player_id();
    if (b == 0 && !req.exact_name().empty()) {
        bool amb = false;
        if (!LoadProfileByName(conn.get(), req.exact_name(), &target, &amb)) {
            rollback();
            Fail(rsp, amb ? "ERR_NAME_AMBIGUOUS" : "ERR_PLAYER_NOT_FOUND",
                 amb ? "ambiguous" : "not found");
            SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "APPLY", *rsp);
            return;
        }
        b = target.player_id();
    } else if (!LoadProfile(conn.get(), b, &target)) {
        rollback();
        Fail(rsp, "ERR_PLAYER_NOT_FOUND", "not found");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    const uint64_t a = req.actor_player_id();
    if (a == b) {
        rollback();
        Fail(rsp, "ERR_CANNOT_ADD_SELF", "self");
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    if (Blocked(conn.get(), a, b)) {
        rollback();
        Fail(rsp, "ERR_ALREADY_BLOCKED", "blocked");
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    if (Blocked(conn.get(), b, a)) {
        rollback();
        rsp->set_privacy_ok(true);
        rsp->set_peer_player_id(b);
        Ok(rsp);
        rsp->set_request_id(0);
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    if (IsFriend(conn.get(), a, b)) {
        rollback();
        Fail(rsp, "ERR_ALREADY_FRIEND", "already friend");
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    const int64_t now = NowUnix();
    const int64_t sent = PendingId(conn.get(), a, b, now, true);
    if (sent > 0) {
        rollback();
        Fail(rsp, "ERR_REQUEST_ALREADY_SENT", "already sent");
        rsp->set_request_id(static_cast<uint64_t>(sent));
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    const int64_t incoming = PendingId(conn.get(), b, a, now, true);
    if (incoming > 0) {
        rollback();
        Fail(rsp, "ERR_INCOMING_REQUEST_EXISTS", "incoming exists");
        rsp->set_request_id(static_cast<uint64_t>(incoming));
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    const int64_t ac = CountSql(conn.get(), "SELECT COUNT(*) FROM friend_relation WHERE player_id=" +
                                                std::to_string(a));
    const int64_t bc = CountSql(conn.get(), "SELECT COUNT(*) FROM friend_relation WHERE player_id=" +
                                                std::to_string(b));
    if (ac < 0 || bc < 0) {
        rollback();
        Fail(rsp, "ERR_INTERNAL", "count failed");
        return;
    }
    if (ac >= FriendCap()) {
        rollback();
        Fail(rsp, "ERR_FRIEND_LIMIT", "self cap");
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    if (bc >= FriendCap()) {
        rollback();
        Fail(rsp, "ERR_TARGET_FRIEND_LIMIT", "target cap");
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    const int64_t inbox =
        CountSql(conn.get(), "SELECT COUNT(*) FROM friend_request WHERE to_player_id=" +
                                 std::to_string(b) + " AND status=0 AND expire_at>=" +
                                 std::to_string(now));
    const int64_t outbox =
        CountSql(conn.get(), "SELECT COUNT(*) FROM friend_request WHERE from_player_id=" +
                                 std::to_string(a) + " AND status=0 AND expire_at>=" +
                                 std::to_string(now));
    if (inbox >= PendingCap() || outbox >= PendingCap()) {
        rollback();
        Fail(rsp, "ERR_PENDING_LIMIT", "pending cap");
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
        return;
    }
    const int64_t expire = now + static_cast<int64_t>(ExpireDays()) * 86400;
    uint64_t rid = (static_cast<uint64_t>(now) << 20) ^ (a << 4) ^ b;
    if (rid == 0)
        rid = a ^ (b << 1) ^ 1;
    std::string ikey = req.idempotency_key().empty()
                           ? ("n:" + std::to_string(rid))
                           : conn->EscapeSql(req.idempotency_key());
    std::ostringstream ins;
    ins << "INSERT INTO friend_request(request_id,from_player_id,to_player_id,status,created_at,"
           "updated_at,expire_at,idempotency_key) VALUES("
        << rid << "," << a << "," << b << "," << kPending << "," << now << "," << now << ","
        << expire << ",'" << ikey << "')";
    if (!conn->update(ins.str())) {
        rollback();
        Fail(rsp, "ERR_RELATION_CONFLICT", "insert conflict");
        return;
    }
    *rsp->mutable_player() = target;
    rsp->set_request_id(rid);
    rsp->set_peer_player_id(b);
    rsp->set_notify_kind("request");
    gdb::FriendBriefDb actor;
    if (!LoadProfile(conn.get(), a, &actor)) {
        actor.set_player_id(a);
        actor.set_level(1);
    }
    auto *rq = rsp->add_requests();
    rq->set_request_id(rid);
    rq->set_created_at(static_cast<uint64_t>(now));
    rq->set_expire_at(static_cast<uint64_t>(expire));
    *rq->mutable_applicant() = actor;
    Ok(rsp);
    SaveIdempotency(conn.get(), a, req.idempotency_key(), "APPLY", *rsp);
    if (!conn->commit()) {
        rollback();
        Fail(rsp, "ERR_INTERNAL", "commit failed");
    }
}

void FriendStore::RequestList(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    const int64_t now = NowUnix();
    const uint64_t after = CursorId(req.cursor());
    const uint32_t lim = PageSize(req, 20, 50);
    std::ostringstream os;
    os << "SELECT r.request_id,r.from_player_id,IFNULL(p.player_name,''),r.created_at,r.expire_at "
          "FROM friend_request r LEFT JOIN player_profile p ON p.player_id=r.from_player_id "
          "WHERE r.to_player_id="
       << req.actor_player_id() << " AND r.status=" << kPending << " AND r.expire_at>=" << now;
    if (after != 0)
        os << " AND r.request_id<" << after;
    os << " ORDER BY r.request_id DESC LIMIT " << (lim + 1);
    MYSQL_RES *res = conn->query(os.str());
    if (!res) {
        Fail(rsp, "ERR_INTERNAL", "request list failed");
        return;
    }
    std::vector<gdb::FriendRequestDb> rows;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        gdb::FriendRequestDb it;
        it.set_request_id(ParseU64(row[0]));
        it.mutable_applicant()->set_player_id(ParseU64(row[1]));
        it.mutable_applicant()->set_name(row[2] ? row[2] : "");
        it.mutable_applicant()->set_level(1);
        it.set_created_at(ParseU64(row[3]));
        it.set_expire_at(ParseU64(row[4]));
        rows.push_back(it);
    }
    mysql_free_result(res);
    bool more = rows.size() > lim;
    if (more)
        rows.resize(lim);
    for (auto &it : rows)
        *rsp->add_requests() = it;
    if (more && !rows.empty())
        rsp->set_next_cursor(std::to_string(rows.back().request_id()));
    Ok(rsp);
}

void FriendStore::Accept(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    if (!req.idempotency_key().empty() &&
        LoadIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), rsp)) {
        if (rsp->ok() && rsp->peer_player_id() != 0)
            LoadProfile(conn.get(), rsp->peer_player_id(), rsp->mutable_player());
        return;
    }
    if (req.request_id() == 0) {
        Fail(rsp, "ERR_INVALID_ARGUMENT", "request_id required");
        return;
    }
    if (!conn->begin()) {
        Fail(rsp, "ERR_INTERNAL", "begin failed");
        return;
    }
    std::ostringstream sel;
    sel << "SELECT request_id,from_player_id,to_player_id,status,expire_at FROM friend_request "
           "WHERE request_id="
        << req.request_id() << " FOR UPDATE";
    MYSQL_RES *res = conn->query(sel.str());
    if (!res) {
        conn->rollback();
        Fail(rsp, "ERR_INTERNAL", "select failed");
        return;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        conn->rollback();
        Fail(rsp, "ERR_REQUEST_NOT_FOUND", "not found");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "ACCEPT", *rsp);
        return;
    }
    const uint64_t from = ParseU64(row[1]);
    const uint64_t to = ParseU64(row[2]);
    const int status = row[3] ? std::atoi(row[3]) : 0;
    const int64_t expire = row[4] ? static_cast<int64_t>(ParseU64(row[4])) : 0;
    mysql_free_result(res);
    if (to != req.actor_player_id()) {
        conn->rollback();
        Fail(rsp, "ERR_REQUEST_NOT_FOUND", "not recipient");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "ACCEPT", *rsp);
        return;
    }
    if (status == kAccepted && IsFriend(conn.get(), to, from)) {
        gdb::FriendBriefDb brief;
        LoadProfile(conn.get(), from, &brief);
        *rsp->mutable_player() = brief;
        rsp->set_peer_player_id(from);
        rsp->set_request_id(req.request_id());
        Ok(rsp);
        rsp->set_idempotent_hit(true);
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "ACCEPT", *rsp);
        conn->commit();
        return;
    }
    if (status != kPending) {
        conn->rollback();
        Fail(rsp, "ERR_REQUEST_NOT_FOUND", "not pending");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "ACCEPT", *rsp);
        return;
    }
    if (expire < NowUnix()) {
        conn->update("UPDATE friend_request SET status=" + std::to_string(kExpired) +
                     ",updated_at=" + std::to_string(NowUnix()) +
                     " WHERE request_id=" + std::to_string(req.request_id()));
        conn->commit();
        Fail(rsp, "ERR_REQUEST_EXPIRED", "expired");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "ACCEPT", *rsp);
        return;
    }
    if (Blocked(conn.get(), to, from) || Blocked(conn.get(), from, to)) {
        conn->rollback();
        Fail(rsp, "ERR_RELATION_CONFLICT", "blocked");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "ACCEPT", *rsp);
        return;
    }
    const int64_t ac = CountSql(conn.get(), "SELECT COUNT(*) FROM friend_relation WHERE player_id=" +
                                                std::to_string(to));
    const int64_t bc = CountSql(conn.get(), "SELECT COUNT(*) FROM friend_relation WHERE player_id=" +
                                                std::to_string(from));
    if (ac >= FriendCap()) {
        conn->rollback();
        Fail(rsp, "ERR_FRIEND_LIMIT", "self cap");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "ACCEPT", *rsp);
        return;
    }
    if (bc >= FriendCap()) {
        conn->rollback();
        Fail(rsp, "ERR_TARGET_FRIEND_LIMIT", "target cap");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "ACCEPT", *rsp);
        return;
    }
    const int64_t now = NowUnix();
    std::ostringstream ins;
    ins << "INSERT INTO friend_relation(player_id,friend_player_id,remark,source,created_at,"
           "updated_at) VALUES("
        << to << "," << from << ",'',0," << now << "," << now << "),(" << from << "," << to
        << ",'',0," << now << "," << now << ")";
    if (!conn->update(ins.str())) {
        conn->rollback();
        Fail(rsp, "ERR_RELATION_CONFLICT", "insert friend failed");
        return;
    }
    conn->update("UPDATE friend_request SET status=" + std::to_string(kAccepted) +
                 ",updated_at=" + std::to_string(now) +
                 " WHERE request_id=" + std::to_string(req.request_id()));
    conn->update("UPDATE friend_request SET status=" + std::to_string(kCanceled) +
                 ",updated_at=" + std::to_string(now) + " WHERE status=" +
                 std::to_string(kPending) + " AND ((from_player_id=" + std::to_string(from) +
                 " AND to_player_id=" + std::to_string(to) + ") OR (from_player_id=" +
                 std::to_string(to) + " AND to_player_id=" + std::to_string(from) +
                 ")) AND request_id<>" + std::to_string(req.request_id()));
    gdb::FriendBriefDb brief;
    LoadProfile(conn.get(), from, &brief);
    *rsp->mutable_player() = brief;
    rsp->set_peer_player_id(from);
    rsp->set_request_id(req.request_id());
    rsp->set_notify_kind("added");
    gdb::FriendBriefDb accepter;
    if (!LoadProfile(conn.get(), to, &accepter)) {
        accepter.set_player_id(to);
        accepter.set_level(1);
    }
    *rsp->add_requests()->mutable_applicant() = accepter;
    Ok(rsp);
    SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "ACCEPT", *rsp);
    if (!conn->commit()) {
        conn->rollback();
        Fail(rsp, "ERR_INTERNAL", "commit failed");
    }
}

void FriendStore::Reject(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    if (!req.idempotency_key().empty() &&
        LoadIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), rsp))
        return;
    if (req.request_id() == 0) {
        Fail(rsp, "ERR_INVALID_ARGUMENT", "request_id required");
        return;
    }
    if (!conn->begin()) {
        Fail(rsp, "ERR_INTERNAL", "begin failed");
        return;
    }
    std::ostringstream sel;
    sel << "SELECT to_player_id,status FROM friend_request WHERE request_id=" << req.request_id()
        << " FOR UPDATE";
    MYSQL_RES *res = conn->query(sel.str());
    if (!res) {
        conn->rollback();
        Fail(rsp, "ERR_INTERNAL", "select failed");
        return;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        conn->rollback();
        Fail(rsp, "ERR_REQUEST_NOT_FOUND", "not found");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "REJECT", *rsp);
        return;
    }
    const uint64_t to = ParseU64(row[0]);
    const int status = row[1] ? std::atoi(row[1]) : 0;
    mysql_free_result(res);
    if (to != req.actor_player_id()) {
        conn->rollback();
        Fail(rsp, "ERR_REQUEST_NOT_FOUND", "not recipient");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "REJECT", *rsp);
        return;
    }
    if (status == kRejected) {
        Ok(rsp);
        rsp->set_idempotent_hit(true);
        rsp->set_request_id(req.request_id());
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "REJECT", *rsp);
        conn->commit();
        return;
    }
    if (status != kPending) {
        conn->rollback();
        Fail(rsp, "ERR_REQUEST_NOT_FOUND", "not pending");
        SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "REJECT", *rsp);
        return;
    }
    conn->update("UPDATE friend_request SET status=" + std::to_string(kRejected) +
                 ",updated_at=" + std::to_string(NowUnix()) +
                 " WHERE request_id=" + std::to_string(req.request_id()));
    rsp->set_request_id(req.request_id());
    Ok(rsp);
    SaveIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), "REJECT", *rsp);
    if (!conn->commit()) {
        conn->rollback();
        Fail(rsp, "ERR_INTERNAL", "commit failed");
    }
}

void FriendStore::DeleteFriend(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    if (!req.idempotency_key().empty() &&
        LoadIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), rsp))
        return;
    const uint64_t a = req.actor_player_id();
    const uint64_t b = req.target_player_id();
    if (b == 0 || a == b) {
        Fail(rsp, "ERR_INVALID_ARGUMENT", "target required");
        return;
    }
    if (!conn->begin()) {
        Fail(rsp, "ERR_INTERNAL", "begin failed");
        return;
    }
    const bool was = IsFriend(conn.get(), a, b);
    conn->update("DELETE FROM friend_relation WHERE (player_id=" + std::to_string(a) +
                 " AND friend_player_id=" + std::to_string(b) + ") OR (player_id=" +
                 std::to_string(b) + " AND friend_player_id=" + std::to_string(a) + ")");
    rsp->set_peer_player_id(b);
    if (was)
        rsp->set_notify_kind("removed");
    Ok(rsp);
    SaveIdempotency(conn.get(), a, req.idempotency_key(), "DELETE", *rsp);
    if (!conn->commit()) {
        conn->rollback();
        Fail(rsp, "ERR_INTERNAL", "commit failed");
    }
}

void FriendStore::Block(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    if (!req.idempotency_key().empty() &&
        LoadIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), rsp))
        return;
    const uint64_t a = req.actor_player_id();
    const uint64_t b = req.target_player_id();
    if (b == 0 || a == b) {
        Fail(rsp, a == b ? "ERR_CANNOT_ADD_SELF" : "ERR_INVALID_ARGUMENT", "target");
        return;
    }
    gdb::FriendBriefDb t;
    if (!LoadProfile(conn.get(), b, &t)) {
        Fail(rsp, "ERR_PLAYER_NOT_FOUND", "not found");
        return;
    }
    if (!conn->begin()) {
        Fail(rsp, "ERR_INTERNAL", "begin failed");
        return;
    }
    const int64_t n =
        CountSql(conn.get(), "SELECT COUNT(*) FROM friend_block WHERE player_id=" + std::to_string(a));
    const bool already = Blocked(conn.get(), a, b);
    if (!already && n >= BlockCap()) {
        conn->rollback();
        Fail(rsp, "ERR_PENDING_LIMIT", "block cap");
        SaveIdempotency(conn.get(), a, req.idempotency_key(), "BLOCK", *rsp);
        return;
    }
    const bool was_friend = IsFriend(conn.get(), a, b);
    const int64_t now = NowUnix();
    conn->update("INSERT IGNORE INTO friend_block(player_id,blocked_player_id,created_at) VALUES(" +
                 std::to_string(a) + "," + std::to_string(b) + "," + std::to_string(now) + ")");
    conn->update("DELETE FROM friend_relation WHERE (player_id=" + std::to_string(a) +
                 " AND friend_player_id=" + std::to_string(b) + ") OR (player_id=" +
                 std::to_string(b) + " AND friend_player_id=" + std::to_string(a) + ")");
    conn->update("UPDATE friend_request SET status=" + std::to_string(kCanceled) +
                 ",updated_at=" + std::to_string(now) + " WHERE status=" +
                 std::to_string(kPending) + " AND ((from_player_id=" + std::to_string(a) +
                 " AND to_player_id=" + std::to_string(b) + ") OR (from_player_id=" +
                 std::to_string(b) + " AND to_player_id=" + std::to_string(a) + "))");
    rsp->set_peer_player_id(b);
    if (was_friend)
        rsp->set_notify_kind("removed");
    Ok(rsp);
    SaveIdempotency(conn.get(), a, req.idempotency_key(), "BLOCK", *rsp);
    if (!conn->commit()) {
        conn->rollback();
        Fail(rsp, "ERR_INTERNAL", "commit failed");
    }
}

void FriendStore::Unblock(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    if (!req.idempotency_key().empty() &&
        LoadIdempotency(conn.get(), req.actor_player_id(), req.idempotency_key(), rsp))
        return;
    const uint64_t a = req.actor_player_id();
    const uint64_t b = req.target_player_id();
    if (b == 0) {
        Fail(rsp, "ERR_INVALID_ARGUMENT", "target required");
        return;
    }
    conn->update("DELETE FROM friend_block WHERE player_id=" + std::to_string(a) +
                 " AND blocked_player_id=" + std::to_string(b));
    rsp->set_peer_player_id(b);
    Ok(rsp);
    SaveIdempotency(conn.get(), a, req.idempotency_key(), "UNBLOCK", *rsp);
}

void FriendStore::BlockList(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp) {
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        Fail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "mysql unavailable");
        return;
    }
    const uint64_t after = CursorId(req.cursor());
    const uint32_t lim = PageSize(req, 100, 100);
    std::ostringstream os;
    os << "SELECT b.blocked_player_id,IFNULL(p.player_name,'') FROM friend_block b LEFT JOIN "
          "player_profile p ON p.player_id=b.blocked_player_id WHERE b.player_id="
       << req.actor_player_id();
    if (after != 0)
        os << " AND b.blocked_player_id>" << after;
    os << " ORDER BY b.blocked_player_id ASC LIMIT " << (lim + 1);
    MYSQL_RES *res = conn->query(os.str());
    if (!res) {
        Fail(rsp, "ERR_INTERNAL", "block list failed");
        return;
    }
    std::vector<std::pair<uint64_t, std::string>> rows;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
        rows.emplace_back(ParseU64(row[0]), row[1] ? row[1] : "");
    mysql_free_result(res);
    bool more = rows.size() > lim;
    if (more)
        rows.resize(lim);
    for (const auto &p : rows)
        FillBrief(rsp->add_friends(), p.first, p.second, "");
    if (more && !rows.empty())
        rsp->set_next_cursor(std::to_string(rows.back().first));
    Ok(rsp);
}
