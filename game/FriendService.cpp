#include "FriendService.h"

#include "Logging.h"
#include "game.pb.h"

#ifdef WEBSERVER_ENABLE_BRPC
#include "BrpcGameDbRepository.h"
#include "GatewayPushClient.h"
#include "gamedb.pb.h"
#include "gateway_push.pb.h"
#endif
#ifdef WEBSERVER_ENABLE_REDIS
#include "SessionStore.h"
#endif

#include <chrono>
#include <map>
#include <string>
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
    }
}
#endif

#ifdef WEBSERVER_ENABLE_BRPC
bool CallFriendOp(const gdb::FriendOpReq &req, gdb::FriendOpRsp *rsp, std::string *err) {
    if (BrpcGameDbRepository::Instance().started())
        return BrpcGameDbRepository::Instance().FriendOp(req, rsp, err);
    if (err)
        *err = "gamedb unavailable";
    if (rsp) {
        rsp->set_ok(false);
        rsp->set_error_code("ERR_DEPENDENCY_UNAVAILABLE");
        rsp->set_message("gamedb unavailable");
    }
    return false;
}

void PushToPlayer(uint64_t player_id, const std::string &message_type, bool reliable,
                  const game::GameResponse &inner) {
#ifdef WEBSERVER_ENABLE_REDIS
    SessionStore::OnlinePushTarget t;
    if (!SessionStore::Instance().GetOnlinePushTarget(player_id, &t))
        return;
    std::string payload;
    if (!inner.SerializeToString(&payload))
        return;
    gwpush::PushBatchRequest preq;
    preq.set_gateway_instance_id(t.gateway_id);
    auto *m = preq.add_messages();
    m->set_player_id(t.player_id);
    m->set_session_id(t.session_id);
    m->set_server_seq(0);
    m->set_message_type(message_type);
    m->set_payload(payload);
    m->set_reliable(reliable);
    m->set_coalescable(!reliable);
    m->set_fence_token(t.fence_token);
    m->set_generation(t.generation);
    gwpush::PushBatchResponse prsp;
    if (!GatewayPushClient::Instance().PushBatch(t.gateway_id, preq, &prsp) || !prsp.ok()) {
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

bool FriendService::HandleSearch(const game::FriendSearchReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_search();
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
#ifdef WEBSERVER_ENABLE_REDIS
    if (SessionStore::Instance().Available() &&
        !SessionStore::Instance().ConsumeFriendApplyQuota(req.player_id())) {
        body->set_ok(false);
        body->set_error_code("ERR_OPERATION_TOO_FREQUENT");
        body->set_message("rate limited");
        FillFail(rsp, "ERR_OPERATION_TOO_FREQUENT", "rate limited");
        rsp->set_retryable(true);
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
        p->set_request_id(orsp.request_id());
        p->set_created_at(static_cast<uint64_t>(NowMs() / 1000));
        if (orsp.has_player()) {
            /* applicant is actor */
        }
        auto *ap = p->mutable_applicant();
        ap->set_player_id(req.player_id());
        PushToPlayer(orsp.peer_player_id(), "friend.request.v1", true, inner);
    }
#endif
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
        p->mutable_peer()->set_player_id(req.player_id());
        PushToPlayer(orsp.peer_player_id(), "friend.added.v1", true, inner);
    }
#endif
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    return orsp.ok();
}

bool FriendService::HandleReject(const game::FriendRejectReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_reject();
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
    return orsp.ok();
}

bool FriendService::HandleDelete(const game::FriendDeleteReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_delete();
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
    }
#endif
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    return orsp.ok();
}

bool FriendService::HandleBlock(const game::FriendBlockReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_block();
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
    }
#endif
    rsp->set_ok(orsp.ok());
    rsp->set_error_code(orsp.error_code());
    rsp->set_message(orsp.message());
    return orsp.ok();
}

bool FriendService::HandleUnblock(const game::FriendUnblockReq &req, game::GameResponse *rsp) {
    auto *body = rsp->mutable_friend_unblock();
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

void FriendService::FanoutPresence(uint64_t player_id, bool online) {
#ifdef WEBSERVER_ENABLE_BRPC
#ifdef WEBSERVER_ENABLE_REDIS
    if (player_id == 0)
        return;
    std::vector<uint64_t> ids;
    if (!SessionStore::Instance().ListFriendIdsFromCache(player_id, &ids) || ids.empty())
        return;
    std::vector<SessionStore::PublicPresence> pres;
    if (!SessionStore::Instance().BatchQueryPublicPresence(ids, &pres))
        return;
    const uint64_t now = static_cast<uint64_t>(NowMs() / 1000);
    game::GameResponse inner;
    inner.set_ok(true);
    auto *p = inner.mutable_friend_presence_push();
    p->set_friend_player_id(player_id);
    p->set_online(online);
    p->set_last_online_time(now);
    for (size_t i = 0; i < ids.size() && i < pres.size(); ++i) {
        if (pres[i].state != "online")
            continue;
        PushToPlayer(ids[i], "friend.presence.v1", false, inner);
    }
#else
    (void)player_id;
    (void)online;
#endif
#else
    (void)player_id;
    (void)online;
#endif
}

#endif  // WEBSERVER_ENABLE_BRPC
