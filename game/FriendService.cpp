#include "FriendService.h"

#include "FriendPresenceBatch.h"
#include "Logging.h"
#include "ServerStats.h"
#include "game.pb.h"

#ifdef WEBSERVER_ENABLE_BRPC
#include "BrpcGameDbRepository.h"
#include "GatewayPushClient.h"
#include "gamedb.pb.h"
#include "gateway_push.pb.h"
#endif
#ifdef WEBSERVER_ENABLE_REDIS
#include "PushReplayStore.h"
#include "SessionStore.h"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

#ifdef WEBSERVER_ENABLE_BRPC
int64_t NowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void CopyBrief(const gdb::FriendBriefDb &in, game::FriendBrief *out) {
    if (!out)
        return;
    out->set_player_id(in.player_id());
    out->set_name(in.name());
    out->set_level(in.level());
    out->set_profession(in.profession());
    out->set_avatar(in.avatar());
    out->set_last_online_time(in.last_online_time());
    out->set_remark(in.remark());
}
#endif

#if defined(WEBSERVER_ENABLE_REDIS) && defined(WEBSERVER_ENABLE_BRPC)
void AttachOnline(std::vector<game::FriendBrief *> briefs) {
    std::vector<uint64_t> ids;
    ids.reserve(briefs.size());
    for (auto *b : briefs) {
        if (b)
            ids.push_back(b->player_id());
    }
    std::vector<SessionStore::PublicPresence> pres;
    if (!SessionStore::Instance().BatchQueryPublicPresence(ids, &pres) ||
        pres.size() != ids.size()) {
        return;
    }
    for (size_t i = 0; i < briefs.size(); ++i) {
        if (!briefs[i])
            continue;
        const auto &st = pres[i].state;
        const bool on = (st == "online" || st == "disconnected");
        briefs[i]->set_online(on);
        if (on) {
            const int64_t seen = pres[i].last_online_unix;
            briefs[i]->set_last_online_time(
                seen > 0 ? static_cast<uint64_t>(seen)
                         : static_cast<uint64_t>(NowMs() / 1000));
        }
    }
    std::vector<uint64_t> offline_ids;
    std::vector<game::FriendBrief *> offline_briefs;
    for (size_t i = 0; i < briefs.size(); ++i) {
        if (!briefs[i] || briefs[i]->online() || briefs[i]->player_id() == 0)
            continue;
        offline_ids.push_back(briefs[i]->player_id());
        offline_briefs.push_back(briefs[i]);
    }
    std::vector<int64_t> seen;
    if (!offline_ids.empty() && SessionStore::Instance().BatchLastSeen(offline_ids, &seen)) {
        for (size_t i = 0; i < offline_briefs.size() && i < seen.size(); ++i) {
            if (seen[i] > 0)
                offline_briefs[i]->set_last_online_time(static_cast<uint64_t>(seen[i]));
        }
    }
}
#endif

#ifdef WEBSERVER_ENABLE_BRPC
int FriendPageLimit() {
    const char *e = std::getenv("GAMEMESH_FRIEND_MAX_COUNT");
    int v = (e && *e) ? std::atoi(e) : 100;
    if (v <= 0)
        v = 100;
    if (v > 500)
        v = 500;
    return v;
}

bool CallFriendOp(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp, std::string *err) {
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = false;
    if (BrpcGameDbRepository::Instance().started())
        ok = BrpcGameDbRepository::Instance().FriendOp(req, rsp, err);
    else {
        if (err)
            *err = "gamedb unavailable";
        if (rsp) {
            rsp->set_ok(false);
            rsp->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
            rsp->set_message("gamedb unavailable");
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    ServerStats::friend_gamedb_latency_ms_sum.fetch_add(static_cast<uint64_t>(ms),
                                                        std::memory_order_relaxed);
    ServerStats::friend_gamedb_latency_count.fetch_add(1, std::memory_order_relaxed);
    if (req.op() == "LIST") {
        ServerStats::friend_list_latency_ms_sum.fetch_add(static_cast<uint64_t>(ms),
                                                          std::memory_order_relaxed);
        ServerStats::friend_list_latency_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (!ok || (rsp && !rsp->ok() && rsp->error_code() == "ERR_DEPENDENCY_UNAVAILABLE"))
        ServerStats::friend_gamedb_error.fetch_add(1, std::memory_order_relaxed);
    if (rsp && rsp->error_code() == "ERR_RELATION_CONFLICT")
        ServerStats::friend_transaction_conflict.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

bool LoadAllFriendIds(uint64_t player_id, std::vector<uint64_t> *out) {
    if (!out || player_id == 0)
        return false;
    out->clear();
    std::string cursor;
    const int page = FriendPageLimit();
    for (int i = 0; i < 8; ++i) {
        gdb::FriendOpReq op;
        op.set_op("LIST");
        op.set_actor_player_id(player_id);
        op.set_page_size(page);
        if (!cursor.empty())
            op.set_cursor(cursor);
        gdb::FriendOpRsp orsp;
        std::string err;
        if (!CallFriendOp(op, &orsp, &err) || !orsp.ok())
            return false;
        for (int n = 0; n < orsp.friends_size(); ++n) {
            const uint64_t id = orsp.friends(n).player_id();
            if (id != 0)
                out->push_back(id);
        }
        if (orsp.next_cursor().empty() || orsp.friends_size() == 0)
            return true;
        cursor = orsp.next_cursor();
    }
    return true;
}

void NotePush(const std::string &message_type, bool ok) {
    auto bump = [&](std::atomic<uint64_t> &slot) {
        slot.fetch_add(1, std::memory_order_relaxed);
    };
    if (message_type == "friend.request.v1")
        bump(ok ? ServerStats::friend_push_ok_request : ServerStats::friend_push_fail_request);
    else if (message_type == "friend.added.v1")
        bump(ok ? ServerStats::friend_push_ok_added : ServerStats::friend_push_fail_added);
    else if (message_type == "friend.removed.v1")
        bump(ok ? ServerStats::friend_push_ok_removed : ServerStats::friend_push_fail_removed);
    else if (message_type == "friend.presence.v1")
        bump(ok ? ServerStats::friend_push_ok_presence : ServerStats::friend_push_fail_presence);
}

void LogFriend(const char *op, uint64_t actor, uint64_t target, uint64_t request_id,
               const std::string &code) {
    LOG_INFO << "[friend] op=" << op << " actor=" << actor << " target=" << target
             << " request_id=" << request_id << " error=" << code;
}

void FillFail(game::GameResponse *rsp, const char *code, const char *msg);

bool RejectOperationId(const std::string &operation_id, game::GameResponse *rsp) {
    if (!operation_id.empty() && operation_id.size() <= 96)
        return false;
    FillFail(rsp, "ERR_INVALID_ARGUMENT", "operation_id required");
    return true;
}

void PushToPlayer(uint64_t player_id, const std::string &message_type, bool reliable,
                  const game::GameResponse &inner) {
#ifdef WEBSERVER_ENABLE_REDIS
    SessionRecord rec;
    if (!SessionStore::Instance().PeekSession(player_id, &rec) || rec.session_id.empty())
        return;
    const bool online = rec.state == SessionState::Online && !rec.gateway_id.empty();
    const bool hold_for_reconnect =
        reliable && (rec.state == SessionState::Online || rec.state == SessionState::Disconnected);
    std::string payload;
    if (!inner.SerializeToString(&payload))
        return;
    uint64_t seq = 0;
    if (hold_for_reconnect && PushReplayStore::Instance().Available()) {
        seq = PushReplayStore::Instance().AppendReliable(player_id, rec.session_id, message_type,
                                                         payload);
        if (seq == 0) {
            ServerStats::friend_replay_append_fail.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN << "[friend] replay append failed type=" << message_type
                     << " player=" << player_id;
        }
    }
    if (!online)
        return;
    gwpush::PushBatchRequest preq;
    preq.set_gateway_instance_id(rec.gateway_id);
    auto *m = preq.add_messages();
    m->set_player_id(player_id);
    m->set_session_id(rec.session_id);
    m->set_server_seq(seq);
    m->set_message_type(message_type);
    m->set_payload(payload);
    m->set_reliable(reliable && seq != 0);
    m->set_coalescable(!(reliable && seq != 0));
    m->set_fence_token(rec.token);
    m->set_generation(rec.generation);
    gwpush::PushBatchResponse prsp;
    const bool pushed =
        GatewayPushClient::Instance().PushBatch(rec.gateway_id, preq, &prsp) && prsp.ok();
    NotePush(message_type, pushed);
    if (!pushed) {
        LOG_WARN << "[friend] PushBatch failed type=" << message_type << " player=" << player_id
                 << " msg=" << prsp.message();
    }
#else
    (void)player_id;
    (void)message_type;
    (void)reliable;
    (void)inner;
#endif
}
#endif

void FillFail(game::GameResponse *rsp, const char *code, const char *msg) {
    rsp->set_ok(false);
    rsp->set_error_code(code);
    rsp->set_message(msg);
}

}  // namespace

FriendService &FriendService::Instance() {
    static FriendService g;
    return g;
}

#ifdef WEBSERVER_ENABLE_REDIS
namespace {
struct FriendPresenceInstall {
    FriendPresenceInstall() {
        SessionStore::Instance().SetFriendPresenceFn([](uint64_t pid, bool on) {
            FriendService::Instance().FanoutPresence(pid, on);
        });
    }
} g_friend_presence_install;
}  // namespace
#endif

#ifndef WEBSERVER_ENABLE_BRPC
namespace {
bool FriendUnavailable(game::GameResponse *rsp) {
    FillFail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", "gamedb unavailable");
    return false;
}
}  // namespace
bool FriendService::HandleList(const game::FriendListReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_list()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
bool FriendService::HandleSearch(const game::FriendSearchReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_search()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
bool FriendService::HandleApply(const game::FriendApplyReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_apply()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
bool FriendService::HandleRequestList(const game::FriendRequestListReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_request_list()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
bool FriendService::HandleAccept(const game::FriendAcceptReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_accept()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
bool FriendService::HandleReject(const game::FriendRejectReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_reject()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
bool FriendService::HandleDelete(const game::FriendDeleteReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_delete()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
bool FriendService::HandleBlock(const game::FriendBlockReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_block()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
bool FriendService::HandleUnblock(const game::FriendUnblockReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_unblock()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
bool FriendService::HandleBlockList(const game::FriendBlockListReq &, game::GameResponse *rsp) {
    rsp->mutable_friend_block_list()->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
    return FriendUnavailable(rsp);
}
void FriendService::FanoutPresence(uint64_t, bool) {}
void FriendService::StopPresenceFanout() {}
FriendWhisperGate FriendService::GateWhisper(uint64_t, uint64_t, std::string *) {
    return FriendWhisperGate::Deliver;
}
#else

bool FriendService::HandleList(const game::FriendListReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_list();
    gdb::FriendOpReq op;
    op.set_op("LIST");
    op.set_actor_player_id(req.player_id());
    op.set_cursor(req.cursor());
    op.set_page_size(req.page_size());
    gdb::FriendOpRsp orsp;
    std::string err;
    if (!CallFriendOp(op, &orsp, &err) && orsp.error_code().empty()) {
        body->set_ok(false);
        body->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
        body->set_message(err);
        FillFail(rsp, "ERR_DEPENDENCY_UNAVAILABLE", err.c_str());
        return false;
    }
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
    body->set_next_cursor(orsp.next_cursor());
    body->set_friend_n(orsp.friend_n());
    body->set_friend_cap(orsp.friend_cap());
    std::vector<game::FriendBrief *> ptrs;
#ifdef WEBSERVER_ENABLE_BRPC
    for (int i = 0; i < orsp.friends_size(); ++i) {
        auto *b = body->add_friends();
        CopyBrief(orsp.friends(i), b);
        ptrs.push_back(b);
    }
#endif
#ifdef WEBSERVER_ENABLE_REDIS
    AttachOnline(ptrs);
    std::vector<uint64_t> ids;
    for (auto *p : ptrs)
        if (p)
            ids.push_back(p->player_id());
    SessionStore::Instance().ReplaceFriendIdCache(req.player_id(), ids);
#endif
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    return orsp.ok();
}

namespace {
bool SameRealm(uint64_t actor, uint64_t target) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (!SessionStore::Instance().Available() || actor == 0 || target == 0)
        return true;
    SessionRecord actor_rec;
    SessionRecord target_rec;
    if (!SessionStore::Instance().PeekSession(actor, &actor_rec) ||
        !SessionStore::Instance().PeekSession(target, &target_rec))
        return true;
    if (actor_rec.server_id == 0 || target_rec.server_id == 0)
        return true;
    return actor_rec.server_id == target_rec.server_id;
#else
    (void)actor;
    (void)target;
    return true;
#endif
}
}  // namespace

bool FriendService::HandleSearch(const game::FriendSearchReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_search();
    if (req.player_id() == 0 || (req.target_player_id() == 0 && req.exact_name().empty())) {
        body->set_ok(false);
        body->set_error_code("ERR_INVALID_ARGUMENT");
        FillFail(rsp, "ERR_INVALID_ARGUMENT", "target required");
        return false;
    }
    if (req.target_player_id() != 0 && !SameRealm(req.player_id(), req.target_player_id())) {
        LOG_WARN << "[friend] op=search actor=" << req.player_id()
                 << " target=" << req.target_player_id() << " cross-realm";
        body->set_ok(false);
        body->set_error_code("ERR_PLAYER_NOT_FOUND");
        FillFail(rsp, "ERR_PLAYER_NOT_FOUND", "player not found");
        return false;
    }
    gdb::FriendOpReq op;
    op.set_op("SEARCH");
    op.set_actor_player_id(req.player_id());
    op.set_target_player_id(req.target_player_id());
    op.set_exact_name(req.exact_name());
    gdb::FriendOpRsp orsp;
    std::string err;
    CallFriendOp(op, &orsp, &err);
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
    if (orsp.has_player()) {
#ifdef WEBSERVER_ENABLE_BRPC
        CopyBrief(orsp.player(), body->mutable_player());
#ifdef WEBSERVER_ENABLE_REDIS
        std::vector<game::FriendBrief *> ptrs{body->mutable_player()};
        AttachOnline(ptrs);
#endif
#endif
    }
    body->set_relation(static_cast<game::FriendRelationState>(orsp.relation()));
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    return orsp.ok();
}

bool FriendService::HandleApply(const game::FriendApplyReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_apply();
    if (RejectOperationId(req.operation_id(), rsp)) {
        body->set_ok(false);
        body->set_error_code("ERR_INVALID_ARGUMENT");
        return false;
    }
    if (req.player_id() == 0 || (req.target_player_id() == 0 && req.exact_name().empty())) {
        body->set_ok(false);
        body->set_error_code("ERR_INVALID_ARGUMENT");
        FillFail(rsp, "ERR_INVALID_ARGUMENT", "target required");
        return false;
    }
    if (req.target_player_id() != 0 && !SameRealm(req.player_id(), req.target_player_id())) {
        LOG_WARN << "[friend] op=apply actor=" << req.player_id()
                 << " target=" << req.target_player_id() << " cross-realm";
        body->set_ok(false);
        body->set_error_code("ERR_PLAYER_NOT_FOUND");
        FillFail(rsp, "ERR_PLAYER_NOT_FOUND", "player not found");
        return false;
    }
#ifdef WEBSERVER_ENABLE_REDIS
    if (!SessionStore::Instance().ConsumeFriendApplyQuota(req.player_id())) {
        body->set_ok(false);
        body->set_error_code("ERR_OPERATION_TOO_FREQUENT");
        body->set_message("rate limited");
        FillFail(rsp, "ERR_OPERATION_TOO_FREQUENT", "rate limited");
        rsp->set_retryable(true);
        ServerStats::friend_request_denied.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
#endif
    gdb::FriendOpReq op;
    op.set_op("APPLY");
    op.set_actor_player_id(req.player_id());
    op.set_target_player_id(req.target_player_id());
    op.set_exact_name(req.exact_name());
    op.set_idempotency_key(req.operation_id());
    gdb::FriendOpRsp orsp;
    std::string err;
    CallFriendOp(op, &orsp, &err);
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
    body->set_request_id(orsp.request_id());
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
#ifdef WEBSERVER_ENABLE_BRPC
    if (orsp.ok() && orsp.notify_kind() == "request" && orsp.peer_player_id() != 0) {
        game::GameResponse inner;
        inner.set_ok(true);
        inner.set_seq(0);
        auto *p = inner.mutable_friend_request_push();
        if (orsp.requests_size() > 0) {
            const auto &rq = orsp.requests(0);
            p->set_request_id(rq.request_id() != 0 ? rq.request_id() : orsp.request_id());
            p->set_created_at(rq.created_at());
            p->set_expire_at(rq.expire_at());
            CopyBrief(rq.applicant(), p->mutable_applicant());
        } else {
            p->set_request_id(orsp.request_id());
            p->set_created_at(static_cast<uint64_t>(NowMs() / 1000));
        }
        if (p->applicant().player_id() == 0)
            p->mutable_applicant()->set_player_id(req.player_id());
#ifdef WEBSERVER_ENABLE_REDIS
        std::vector<game::FriendBrief *> ptrs{p->mutable_applicant()};
        AttachOnline(ptrs);
#endif
        PushToPlayer(orsp.peer_player_id(), "friend.request.v1", true, inner);
    }
#endif
    if (orsp.ok())
        ServerStats::friend_request_ok.fetch_add(1, std::memory_order_relaxed);
    else if (orsp.error_code() == "ERR_DEPENDENCY_UNAVAILABLE")
        ServerStats::friend_request_error.fetch_add(1, std::memory_order_relaxed);
    else
        ServerStats::friend_request_denied.fetch_add(1, std::memory_order_relaxed);
    LogFriend("apply", req.player_id(), req.target_player_id(), orsp.request_id(), orsp.error_code());
    return orsp.ok();
}

bool FriendService::HandleRequestList(const game::FriendRequestListReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_request_list();
    gdb::FriendOpReq op;
    op.set_op("REQUEST_LIST");
    op.set_actor_player_id(req.player_id());
    op.set_cursor(req.cursor());
    op.set_page_size(req.page_size());
    gdb::FriendOpRsp orsp;
    std::string err;
    CallFriendOp(op, &orsp, &err);
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
    body->set_next_cursor(orsp.next_cursor());
#ifdef WEBSERVER_ENABLE_BRPC
    std::vector<game::FriendBrief *> ptrs;
    for (int i = 0; i < orsp.requests_size(); ++i) {
        auto *it = body->add_requests();
        it->set_request_id(orsp.requests(i).request_id());
        it->set_created_at(orsp.requests(i).created_at());
        it->set_expire_at(orsp.requests(i).expire_at());
        CopyBrief(orsp.requests(i).applicant(), it->mutable_applicant());
        ptrs.push_back(it->mutable_applicant());
    }
#ifdef WEBSERVER_ENABLE_REDIS
    AttachOnline(ptrs);
#endif
#endif
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    return orsp.ok();
}

bool FriendService::HandleAccept(const game::FriendAcceptReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_accept();
    if (RejectOperationId(req.operation_id(), rsp)) {
        body->set_ok(false);
        body->set_error_code("ERR_INVALID_ARGUMENT");
        return false;
    }
    gdb::FriendOpReq op;
    op.set_op("ACCEPT");
    op.set_actor_player_id(req.player_id());
    op.set_request_id(req.request_id());
    op.set_idempotency_key(req.operation_id());
    gdb::FriendOpRsp orsp;
    std::string err;
    CallFriendOp(op, &orsp, &err);
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
#ifdef WEBSERVER_ENABLE_BRPC
    if (orsp.has_player()) {
        CopyBrief(orsp.player(), body->mutable_peer());
#ifdef WEBSERVER_ENABLE_REDIS
        std::vector<game::FriendBrief *> ptrs{body->mutable_peer()};
        AttachOnline(ptrs);
#endif
    }
    if (orsp.ok() && orsp.notify_kind() == "added" && orsp.peer_player_id() != 0) {
        game::GameResponse inner;
        inner.set_ok(true);
        auto *p = inner.mutable_friend_added_push();
        if (orsp.requests_size() > 0 && orsp.requests(0).has_applicant())
            CopyBrief(orsp.requests(0).applicant(), p->mutable_peer());
        else
            p->mutable_peer()->set_player_id(req.player_id());
        if (p->peer().player_id() == 0)
            p->mutable_peer()->set_player_id(req.player_id());
#ifdef WEBSERVER_ENABLE_REDIS
        std::vector<game::FriendBrief *> ptrs{p->mutable_peer()};
        AttachOnline(ptrs);
        SessionStore::Instance().AddFriendIdCache(req.player_id(), orsp.peer_player_id());
        SessionStore::Instance().AddFriendIdCache(orsp.peer_player_id(), req.player_id());
#endif
        PushToPlayer(orsp.peer_player_id(), "friend.added.v1", true, inner);
    }
#endif
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    if (orsp.ok())
        ServerStats::friend_accept_total.fetch_add(1, std::memory_order_relaxed);
    LogFriend("accept", req.player_id(), orsp.peer_player_id(), req.request_id(), orsp.error_code());
    return orsp.ok();
}

bool FriendService::HandleReject(const game::FriendRejectReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_reject();
    if (RejectOperationId(req.operation_id(), rsp)) {
        body->set_ok(false);
        body->set_error_code("ERR_INVALID_ARGUMENT");
        return false;
    }
    gdb::FriendOpReq op;
    op.set_op("REJECT");
    op.set_actor_player_id(req.player_id());
    op.set_request_id(req.request_id());
    op.set_idempotency_key(req.operation_id());
    gdb::FriendOpRsp orsp;
    std::string err;
    CallFriendOp(op, &orsp, &err);
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    LogFriend("reject", req.player_id(), 0, req.request_id(), orsp.error_code());
    return orsp.ok();
}

bool FriendService::HandleDelete(const game::FriendDeleteReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_delete();
    if (RejectOperationId(req.operation_id(), rsp)) {
        body->set_ok(false);
        body->set_error_code("ERR_INVALID_ARGUMENT");
        return false;
    }
    gdb::FriendOpReq op;
    op.set_op("DELETE");
    op.set_actor_player_id(req.player_id());
    op.set_target_player_id(req.friend_player_id());
    op.set_idempotency_key(req.operation_id());
    gdb::FriendOpRsp orsp;
    std::string err;
    CallFriendOp(op, &orsp, &err);
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
#ifdef WEBSERVER_ENABLE_BRPC
    if (orsp.ok() && orsp.notify_kind() == "removed" && orsp.peer_player_id() != 0) {
        game::GameResponse inner;
        inner.set_ok(true);
        inner.mutable_friend_removed_push()->set_friend_player_id(req.player_id());
        PushToPlayer(orsp.peer_player_id(), "friend.removed.v1", true, inner);
#ifdef WEBSERVER_ENABLE_REDIS
        SessionStore::Instance().RemoveFriendIdCache(req.player_id(), orsp.peer_player_id());
        SessionStore::Instance().RemoveFriendIdCache(orsp.peer_player_id(), req.player_id());
#endif
    }
#endif
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    if (orsp.ok())
        ServerStats::friend_delete_total.fetch_add(1, std::memory_order_relaxed);
    LogFriend("delete", req.player_id(), req.friend_player_id(), 0, orsp.error_code());
    return orsp.ok();
}

bool FriendService::HandleBlock(const game::FriendBlockReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_block();
    if (RejectOperationId(req.operation_id(), rsp)) {
        body->set_ok(false);
        body->set_error_code("ERR_INVALID_ARGUMENT");
        return false;
    }
    gdb::FriendOpReq op;
    op.set_op("BLOCK");
    op.set_actor_player_id(req.player_id());
    op.set_target_player_id(req.target_player_id());
    op.set_idempotency_key(req.operation_id());
    gdb::FriendOpRsp orsp;
    std::string err;
    CallFriendOp(op, &orsp, &err);
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
#ifdef WEBSERVER_ENABLE_BRPC
    if (orsp.ok() && orsp.notify_kind() == "removed" && orsp.peer_player_id() != 0) {
        game::GameResponse inner;
        inner.set_ok(true);
        inner.mutable_friend_removed_push()->set_friend_player_id(req.player_id());
        PushToPlayer(orsp.peer_player_id(), "friend.removed.v1", true, inner);
#ifdef WEBSERVER_ENABLE_REDIS
        SessionStore::Instance().RemoveFriendIdCache(req.player_id(), orsp.peer_player_id());
        SessionStore::Instance().RemoveFriendIdCache(orsp.peer_player_id(), req.player_id());
#endif
    }
#ifdef WEBSERVER_ENABLE_REDIS
    if (orsp.ok()) {
        SessionStore::Instance().InvalidateBlockCache(req.player_id());
        SessionStore::Instance().InvalidateBlockCache(req.target_player_id());
    }
#endif
#endif
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    if (orsp.ok())
        ServerStats::friend_block_total.fetch_add(1, std::memory_order_relaxed);
    LogFriend("block", req.player_id(), req.target_player_id(), 0, orsp.error_code());
    return orsp.ok();
}

bool FriendService::HandleUnblock(const game::FriendUnblockReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_unblock();
    if (RejectOperationId(req.operation_id(), rsp)) {
        body->set_ok(false);
        body->set_error_code("ERR_INVALID_ARGUMENT");
        return false;
    }
    gdb::FriendOpReq op;
    op.set_op("UNBLOCK");
    op.set_actor_player_id(req.player_id());
    op.set_target_player_id(req.target_player_id());
    op.set_idempotency_key(req.operation_id());
    gdb::FriendOpRsp orsp;
    std::string err;
    CallFriendOp(op, &orsp, &err);
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
#ifdef WEBSERVER_ENABLE_REDIS
    if (orsp.ok()) {
        SessionStore::Instance().InvalidateBlockCache(req.player_id());
        SessionStore::Instance().InvalidateBlockCache(req.target_player_id());
    }
#endif
    LogFriend("unblock", req.player_id(), req.target_player_id(), 0, orsp.error_code());
    return orsp.ok();
}

bool FriendService::HandleBlockList(const game::FriendBlockListReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_block_list();
    gdb::FriendOpReq op;
    op.set_op("BLOCK_LIST");
    op.set_actor_player_id(req.player_id());
    op.set_cursor(req.cursor());
    op.set_page_size(req.page_size());
    gdb::FriendOpRsp orsp;
    std::string err;
    CallFriendOp(op, &orsp, &err);
    body->set_ok(orsp.ok());
    body->set_error_code(orsp.error_code());
    body->set_message(orsp.message());
    body->set_next_cursor(orsp.next_cursor());
#ifdef WEBSERVER_ENABLE_BRPC
    for (int i = 0; i < orsp.friends_size(); ++i)
        CopyBrief(orsp.friends(i), body->add_blocked());
#endif
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    return orsp.ok();
}

namespace {
bool LoadBlockSet(uint64_t player_id, std::unordered_set<uint64_t> *out) {
    if (!out || player_id == 0)
        return false;
    out->clear();
#ifdef WEBSERVER_ENABLE_REDIS
    std::vector<uint64_t> cached;
    if (SessionStore::Instance().ListBlockedIdsFromCache(player_id, &cached)) {
        ServerStats::friend_block_gate_cache_hit.fetch_add(1, std::memory_order_relaxed);
        out->insert(cached.begin(), cached.end());
        return true;
    }
#endif
    ServerStats::friend_block_gate_cache_miss.fetch_add(1, std::memory_order_relaxed);
    std::vector<uint64_t> ids;
    std::string cursor;
    const int page = FriendPageLimit();
    for (int i = 0; i < 4; ++i) {
        gdb::FriendOpReq op;
        op.set_op("BLOCK_LIST");
        op.set_actor_player_id(player_id);
        op.set_page_size(page);
        if (!cursor.empty())
            op.set_cursor(cursor);
        gdb::FriendOpRsp orsp;
        std::string err;
        if (!CallFriendOp(op, &orsp, &err) || !orsp.ok())
            return false;
        for (int n = 0; n < orsp.friends_size(); ++n) {
            const uint64_t id = orsp.friends(n).player_id();
            if (id != 0)
                ids.push_back(id);
        }
        if (orsp.next_cursor().empty() || orsp.friends_size() == 0)
            break;
        cursor = orsp.next_cursor();
    }
    out->insert(ids.begin(), ids.end());
#ifdef WEBSERVER_ENABLE_REDIS
    if (!SessionStore::Instance().ReplaceBlockCache(player_id, ids))
        SessionStore::Instance().InvalidateBlockCache(player_id);
#endif
    return true;
}
}  // namespace

FriendWhisperGate FriendService::GateWhisper(uint64_t actor_player_id, uint64_t target_player_id,
                                             std::string *error_code) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (!SessionStore::Instance().Available()) {
        ServerStats::friend_block_gate_degraded.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN << "[friend] whisper gate degraded actor=" << actor_player_id
                 << " target=" << target_player_id;
        return FriendWhisperGate::Deliver;
    }
#endif
    std::unordered_set<uint64_t> actor_blocked;
    std::unordered_set<uint64_t> target_blocked;
    if (!LoadBlockSet(actor_player_id, &actor_blocked) ||
        !LoadBlockSet(target_player_id, &target_blocked)) {
        ServerStats::friend_block_gate_degraded.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN << "[friend] whisper gate degraded actor=" << actor_player_id
                 << " target=" << target_player_id;
        return FriendWhisperGate::Deliver;
    }
    if (actor_blocked.count(target_player_id) != 0) {
        if (error_code)
            *error_code = "ERR_ALREADY_BLOCKED";
        return FriendWhisperGate::Reject;
    }
    if (target_blocked.count(actor_player_id) != 0)
        return FriendWhisperGate::Hide;
    return FriendWhisperGate::Deliver;
}

namespace {
int EnvInt(const char *key, int def, int lo, int hi) {
    const char *e = std::getenv(key);
    int v = (e && *e) ? std::atoi(e) : def;
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

int64_t SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void RunPresenceFanout(uint64_t player_id, bool online) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (player_id == 0)
        return;
    const uint64_t now = static_cast<uint64_t>(NowMs() / 1000);
    SessionStore::Instance().RememberLastSeen(player_id, static_cast<int64_t>(now));
    std::vector<uint64_t> ids;
    if (SessionStore::Instance().ListFriendIdsFromCache(player_id, &ids)) {
        ServerStats::friend_cache_hit.fetch_add(1, std::memory_order_relaxed);
    } else {
        ServerStats::friend_cache_miss.fetch_add(1, std::memory_order_relaxed);
        if (!LoadAllFriendIds(player_id, &ids))
            return;
        if (!SessionStore::Instance().ReplaceFriendIdCache(player_id, ids))
            return;
    }
    if (ids.empty())
        return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<SessionStore::OnlinePushTarget> targets;
    const bool listed = SessionStore::Instance().BatchOnlinePushTargets(ids, &targets);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    ServerStats::friend_online_batch_latency_ms_sum.fetch_add(static_cast<uint64_t>(ms),
                                                              std::memory_order_relaxed);
    ServerStats::friend_online_batch_latency_count.fetch_add(1, std::memory_order_relaxed);
    if (!listed)
        return;
    game::GameResponse inner;
    inner.set_ok(true);
    auto *p = inner.mutable_friend_presence_push();
    p->set_friend_player_id(player_id);
    p->set_online(online);
    p->set_last_online_time(now);
    std::string payload;
    if (!inner.SerializeToString(&payload))
        return;
    std::vector<FriendPresenceDest> dests;
    dests.reserve(targets.size());
    std::unordered_map<uint64_t, SessionStore::OnlinePushTarget> by_id;
    for (const auto &target : targets) {
        FriendPresenceDest dest;
        dest.player_id = target.player_id;
        dest.gateway_id = target.gateway_id;
        dests.push_back(dest);
        by_id[target.player_id] = target;
    }
    const size_t batch_max =
        static_cast<size_t>(EnvInt("GAMEMESH_FRIEND_PRESENCE_BATCH_MAX", 200, 1, 1000));
    const auto batches = SplitPresenceBatches(dests, batch_max);
    for (const auto &batch : batches) {
        ServerStats::friend_presence_batch_size.store(batch.player_ids.size(),
                                                     std::memory_order_relaxed);
        gwpush::PushBatchRequest preq;
        preq.set_gateway_instance_id(batch.gateway_id);
        for (uint64_t id : batch.player_ids) {
            const auto it = by_id.find(id);
            if (it == by_id.end())
                continue;
            auto *m = preq.add_messages();
            m->set_player_id(id);
            m->set_session_id(it->second.session_id);
            m->set_server_seq(0);
            m->set_message_type("friend.presence.v1");
            m->set_payload(payload);
            m->set_reliable(false);
            m->set_coalescable(true);
            m->set_fence_token(it->second.fence_token);
            m->set_generation(it->second.generation);
        }
        if (preq.messages_size() == 0)
            continue;
        gwpush::PushBatchResponse prsp;
        const bool pushed =
            GatewayPushClient::Instance().PushBatch(batch.gateway_id, preq, &prsp) && prsp.ok();
        NotePush("friend.presence.v1", pushed);
        if (!pushed) {
            LOG_WARN << "[friend] presence batch failed gateway=" << batch.gateway_id
                     << " n=" << preq.messages_size();
        }
    }
#else
    (void)player_id;
    (void)online;
#endif
}

struct PresenceFanoutQueue {
    std::mutex mu;
    std::condition_variable cv;
    struct Item {
        bool online = false;
        int64_t deadline_ms = 0;
    };
    std::unordered_map<uint64_t, Item> pending;
    size_t cap = 4096;
    int window_ms = 400;
    bool started = false;
    bool stop = false;
    std::thread worker;
};

PresenceFanoutQueue &PresenceQueue() {
    static PresenceFanoutQueue q;
    return q;
}

void PresenceFanoutLoop() {
    auto &q = PresenceQueue();
    for (;;) {
        std::vector<std::pair<uint64_t, bool>> due;
        {
            std::unique_lock<std::mutex> lk(q.mu);
            for (;;) {
                if (q.stop)
                    return;
                if (q.pending.empty()) {
                    q.cv.wait(lk, [&] { return q.stop || !q.pending.empty(); });
                    continue;
                }
                int64_t next = 0;
                for (const auto &kv : q.pending) {
                    if (next == 0 || kv.second.deadline_ms < next)
                        next = kv.second.deadline_ms;
                }
                const int64_t now = SteadyMs();
                if (now < next) {
                    q.cv.wait_for(lk, std::chrono::milliseconds(next - now));
                    continue;
                }
                for (auto it = q.pending.begin(); it != q.pending.end();) {
                    if (it->second.deadline_ms <= SteadyMs()) {
                        due.emplace_back(it->first, it->second.online);
                        it = q.pending.erase(it);
                    } else {
                        ++it;
                    }
                }
                break;
            }
        }
        for (const auto &item : due)
            RunPresenceFanout(item.first, item.second);
    }
}

void EnsurePresenceFanoutThread() {
    auto &q = PresenceQueue();
    std::lock_guard<std::mutex> lk(q.mu);
    if (q.started || q.stop)
        return;
    q.cap = static_cast<size_t>(EnvInt("GAMEMESH_FRIEND_PRESENCE_QUEUE_MAX", 4096, 1, 100000));
    q.window_ms = EnvInt("GAMEMESH_FRIEND_PRESENCE_WINDOW_MS", 400, 50, 5000);
    q.started = true;
    q.worker = std::thread(PresenceFanoutLoop);
}

void StopPresenceWorker() {
    auto &q = PresenceQueue();
    {
        std::lock_guard<std::mutex> lk(q.mu);
        q.stop = true;
    }
    q.cv.notify_all();
    if (q.worker.joinable())
        q.worker.join();
}
}  // namespace

void FriendService::StopPresenceFanout() { StopPresenceWorker(); }

void FriendService::FanoutPresence(uint64_t player_id, bool online) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (player_id == 0)
        return;
    EnsurePresenceFanoutThread();
    auto &q = PresenceQueue();
    {
        std::lock_guard<std::mutex> lk(q.mu);
        if (q.stop)
            return;
        auto it = q.pending.find(player_id);
        if (it != q.pending.end()) {
            it->second.online = online;
            ServerStats::friend_presence_coalesced.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (q.pending.size() >= q.cap) {
            ServerStats::friend_presence_queue_drop.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        PresenceFanoutQueue::Item item;
        item.online = online;
        item.deadline_ms = SteadyMs() + q.window_ms;
        q.pending.emplace(player_id, item);
    }
    q.cv.notify_one();
#else
    (void)player_id;
    (void)online;
#endif
}

#endif  // WEBSERVER_ENABLE_BRPC
