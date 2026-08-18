#include "GameLogicServiceImpl.h"

#include "ClientSeqGate.h"
#include "FormalMode.h"
#include "ForwardMetaContext.h"
#include "GameLogic.h"
#include "GameService.h"
#include "Logging.h"
#include "MapInstanceRegistry.h"
#include "MapRuntime.h"
#include "MailService.h"
#include "PlayerSerialQueue.h"
#include "game.pb.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "PlacementStore.h"
#endif

#include <brpc/controller.h>

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

std::string FnvChecksum(const std::string &raw) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : raw) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::string ChecksumRuntimeState(const glrpc::PlayerRuntimeState &st) {
    std::string raw;
    st.SerializeToString(&raw);
    return FnvChecksum(raw);
}

struct BoundPlayer {
    std::string session_id;
    std::string fence_token;
    std::string gateway_instance_id;
    uint64_t map_instance_id = 0;
    uint64_t map_owner_epoch = 0;
    uint64_t route_version = 0;
    uint64_t generation = 0;
    uint64_t last_client_seq = 0;
    std::string last_response_frame;
    bool frozen = false;
    std::string transfer_id;
    std::string last_import_checksum;
};

std::mutex g_bound_mu;
std::unordered_map<uint64_t, BoundPlayer> g_bound;

bool LocalLogicOk(const std::string &req_logic, std::string *err) {
    if (req_logic.empty())
        return true;  // 兼容旧客户端未填
    const std::string &local = MapInstanceRegistry::Instance().local_instance_id();
    if (req_logic == local)
        return true;
    if (err)
        *err = "wrong gamelogic owner want=" + local + " got=" + req_logic;
    return false;
}

bool FenceOk(uint64_t player_id, const std::string &session_id, const std::string &fence,
             uint64_t generation, std::string *err) {
    std::lock_guard<std::mutex> lk(g_bound_mu);
    auto it = g_bound.find(player_id);
    if (it == g_bound.end()) {
        if (err)
            *err = "player not bound";
        return false;
    }
    if (!session_id.empty() && it->second.session_id != session_id) {
        if (err)
            *err = "session_id mismatch";
        return false;
    }
    if (it->second.fence_token != fence) {
        if (err)
            *err = "fence_token rejected";
        return false;
    }
    if (generation != 0 && it->second.generation != 0 && generation != it->second.generation) {
        if (err)
            *err = "generation rejected";
        return false;
    }
    return true;
}

bool GetPushTargetLocked(uint64_t player_id, std::string *gateway_instance_id,
                         std::string *session_id) {
    auto it = g_bound.find(player_id);
    if (it == g_bound.end())
        return false;
    if (gateway_instance_id)
        *gateway_instance_id = it->second.gateway_instance_id;
    if (session_id)
        *session_id = it->second.session_id;
    return !it->second.gateway_instance_id.empty() && !it->second.session_id.empty();
}

/** client_seq：0=不检查；==last 幂等；>last 前进（允许 Gateway 本地消息造成的空洞）；<last 拒绝 */
using SeqDecision = ClientSeqDecision;

SeqDecision CheckClientSeq(uint64_t player_id, uint64_t client_seq, std::string *cached_frame,
                           std::string *err) {
    std::lock_guard<std::mutex> lk(g_bound_mu);
    auto it = g_bound.find(player_id);
    if (client_seq != 0 && it == g_bound.end()) {
        if (err)
            *err = "player not bound";
        return SeqDecision::Reject;
    }
    const uint64_t last = (it == g_bound.end()) ? 0 : it->second.last_client_seq;
    const std::string &cached =
        (it == g_bound.end()) ? std::string() : it->second.last_response_frame;
    return EvaluateClientSeq(last, client_seq, cached, cached_frame, err);
}

void CommitClientSeq(uint64_t player_id, uint64_t client_seq, const std::string &frame) {
    if (client_seq == 0)
        return;
    std::lock_guard<std::mutex> lk(g_bound_mu);
    auto it = g_bound.find(player_id);
    if (it == g_bound.end())
        return;
    it->second.last_client_seq = client_seq;
    it->second.last_response_frame = frame;
}

void ExecuteDispatch(const glrpc::ClientCommand &request, glrpc::CommandResult *response) {
    response->Clear();
    std::string err;
    if (!LocalLogicOk(request.gamelogic_instance_id(), &err)) {
        response->set_ok(false);
        response->set_error_code("ERR_WRONG_GAMELOGIC_OWNER");
        response->set_message(err);
        return;
    }
    if (!FenceOk(request.player_id(), request.session_id(), request.fence_token(),
                 request.generation(), &err)) {
        response->set_ok(false);
        response->set_error_code("FENCE_REJECT");
        response->set_message(err);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_bound_mu);
        auto it = g_bound.find(request.player_id());
        if (it != g_bound.end()) {
            if (it->second.frozen) {
                response->set_ok(false);
                response->set_error_code("PLAYER_FROZEN");
                response->set_message("player frozen for transfer");
                return;
            }
            const bool is_enter_map = request.message_type() == "enter_map";
            if (!is_enter_map && request.route_version() != 0 && it->second.route_version != 0 &&
                request.route_version() < it->second.route_version) {
                response->set_ok(false);
                response->set_error_code("ERR_ROUTE_STALE");
                response->set_message("route_version stale");
                return;
            }
        }
    }

    std::string cached;
    const SeqDecision sd = CheckClientSeq(request.player_id(), request.client_seq(), &cached, &err);
    if (sd == SeqDecision::Reject) {
        response->set_ok(false);
        response->set_error_code("ERR_STALE_SEQ");
        response->set_message(err.empty() ? "client_seq out of order" : err);
        return;
    }
    if (sd == SeqDecision::Idempotent) {
        response->set_ok(true);
        response->set_message("idempotent");
        if (!cached.empty())
            response->set_response_frame(cached);
        return;
    }

    if (request.map_instance_id() != 0) {
        const MapWriteFence fence = MapInstanceRegistry::Instance().CheckWrite(
            request.map_instance_id(), request.map_owner_epoch());
        // Formal 禁止隐式 Claim，但仍须放行 enter_map：由 HandleEnterMap 校验权威 Placement 后再 Claim。
        // 本地 lease 过期/缺失时同样放行 enter_map（先 Release 再让 HandleEnterMap 按权威记录 Claim）。
        // 非 Formal 保留空 message_type 兼容旧客户端。
        const bool is_enter_map = request.message_type() == "enter_map" ||
                                  (!FormalModeEnabled() && request.message_type().empty());
        const bool allow_enter_reclaim =
            is_enter_map && (fence == MapWriteFence::NotClaimed ||
                             fence == MapWriteFence::LeaseExpired ||
                             fence == MapWriteFence::LeaseMissing);
        if (allow_enter_reclaim &&
            (fence == MapWriteFence::LeaseExpired || fence == MapWriteFence::LeaseMissing)) {
            MapInstanceRegistry::Instance().Release(request.map_instance_id());
        }
        if (fence != MapWriteFence::Ok && !allow_enter_reclaim) {
            response->set_ok(false);
            if (fence == MapWriteFence::LeaseExpired) {
                response->set_error_code("LEASE_EXPIRED");
                response->set_message("map owner lease expired");
                MapInstanceRegistry::Instance().Release(request.map_instance_id());
            } else if (fence == MapWriteFence::LeaseMissing) {
                response->set_error_code("LEASE_MISSING");
                response->set_message("map lease missing or zero");
            } else if (fence == MapWriteFence::NotClaimed) {
                response->set_error_code("NOT_CLAIMED");
                response->set_message("map not claimed on this logic");
            } else {
                response->set_error_code("STALE_EPOCH");
                response->set_message("stale map_owner_epoch");
            }
            return;
        }
    }

    ForwardRouteMeta meta;
    meta.player_id = request.player_id();
    meta.map_instance_id = request.map_instance_id();
    meta.owner_epoch = request.map_owner_epoch();
    meta.route_version = request.route_version();
    meta.client_seq = request.client_seq();
    meta.gamelogic_instance_id = request.gamelogic_instance_id();
    meta.session_id = request.session_id();
    meta.fence_token = request.fence_token();
    meta.generation = request.generation();
    ForwardMetaContext::Set(meta);

    std::string out_frame;
    const bool ok = gameproto::HandleFrame(request.payload(), &out_frame);
    ForwardMetaContext::Clear();
    response->set_ok(ok);
    response->set_message(ok ? "ok" : "handle_frame_failed");
    if (!out_frame.empty())
        response->set_response_frame(out_frame);
    if (ok)
        CommitClientSeq(request.player_id(), request.client_seq(), out_frame);
}

}  // namespace

bool GameLogicGetPushTarget(uint64_t player_id, std::string *gateway_instance_id,
                            std::string *session_id) {
    std::lock_guard<std::mutex> lk(g_bound_mu);
    return GetPushTargetLocked(player_id, gateway_instance_id, session_id);
}

bool GameLogicGetBoundMeta(uint64_t player_id, std::string *gateway_instance_id,
                           std::string *session_id, std::string *fence_token,
                           uint64_t *generation) {
    std::lock_guard<std::mutex> lk(g_bound_mu);
    auto it = g_bound.find(player_id);
    if (it == g_bound.end())
        return false;
    if (gateway_instance_id)
        *gateway_instance_id = it->second.gateway_instance_id;
    if (session_id)
        *session_id = it->second.session_id;
    if (fence_token)
        *fence_token = it->second.fence_token;
    if (generation)
        *generation = it->second.generation;
    return true;
}

void GameLogicServiceImpl::BindPlayer(::google::protobuf::RpcController *controller,
                                      const ::glrpc::BindPlayerRequest *request,
                                      ::glrpc::BindPlayerResponse *response,
                                      ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0 || request->fence_token().empty()) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        response->set_message("player_id/fence_token required");
        return;
    }
    std::string err;
    if (!LocalLogicOk(request->gamelogic_instance_id(), &err)) {
        response->set_ok(false);
        response->set_error_code("ERR_WRONG_GAMELOGIC_OWNER");
        response->set_message(err);
        return;
    }
    const uint64_t player_id = request->player_id();
    BoundPlayer bp;
    bp.session_id = request->session_id();
    bp.fence_token = request->fence_token();
    bp.gateway_instance_id = request->gateway_instance_id();
    bp.map_instance_id = request->map_instance_id();
    bp.map_owner_epoch = request->map_owner_epoch();
    bp.route_version = request->route_version();
    bp.generation = request->generation();
    bp.transfer_id = request->transfer_id();
    bp.frozen = false;
    bp.last_client_seq = 0;
    {
        std::lock_guard<std::mutex> lk(g_bound_mu);
        auto it = g_bound.find(player_id);
        if (it != g_bound.end() && bp.generation != 0 && it->second.generation != 0 &&
            bp.generation < it->second.generation) {
            response->set_ok(false);
            response->set_error_code("STALE_GENERATION");
            response->set_message("stale bind generation");
            return;
        }
        if (it != g_bound.end() && !bp.transfer_id.empty() &&
            it->second.transfer_id == bp.transfer_id && !it->second.frozen) {
            response->set_ok(true);
            response->set_message("prepare idempotent");
            response->set_bag_item_kinds(0);
            game::PlayerAttributes attrs;
            if (GameLogic::Instance().GetPlayerAttributes(player_id, &attrs)) {
                std::string pb;
                if (attrs.SerializeToString(&pb))
                    response->set_profile_pb(pb);
            }
            return;
        }
        // 新 fence/session：重置 client_seq
        g_bound[player_id] = bp;
    }
    done_guard.release();
    auto *rsp = response;
    auto *closure = done;
    const auto req_copy = *request;
    if (!PlayerSerialQueue::Instance().TryPost(player_id, [player_id, req_copy, rsp, closure]() {
            brpc::ClosureGuard g(closure);
            std::string load_err;
            game::PlayerAttributes attrs;
            if (!GameLogic::Instance().BindAuthenticatedPlayer(player_id, &load_err, &attrs)) {
                {
                    std::lock_guard<std::mutex> lk(g_bound_mu);
                    g_bound.erase(player_id);
                }
                rsp->set_ok(false);
                rsp->set_error_code("ERR_PLAYER_LOAD_FAILED");
                rsp->set_message(load_err.empty() ? "player load failed" : load_err);
                return;
            }
            rsp->set_ok(true);
            rsp->set_message("player ready");
            rsp->set_bag_item_kinds(0);
            {
                std::string pb;
                if (attrs.SerializeToString(&pb))
                    rsp->set_profile_pb(pb);
            }
            LOG_INFO << "BindPlayer ok player_id=" << player_id
                     << " session=" << req_copy.session_id() << " gen=" << req_copy.generation()
                     << " gw=" << req_copy.gateway_instance_id();
            AoiPushBatch pushes;
            MapEntity self;
            std::vector<MapEntity> snap;
            if (MapRuntime::Instance().Reconnect(player_id, req_copy.gateway_instance_id(),
                                                 req_copy.session_id(), &self, &snap, &pushes))
                GameLogic::Instance().EmitAoi(pushes);
        })) {
        brpc::ClosureGuard g(closure);
        {
            std::lock_guard<std::mutex> lk(g_bound_mu);
            g_bound.erase(player_id);
        }
        rsp->set_ok(false);
        rsp->set_error_code("ERR_OVERLOAD");
        rsp->set_message("player serial queue full");
    }
}

void GameLogicServiceImpl::Dispatch(::google::protobuf::RpcController *controller,
                                    const ::glrpc::ClientCommand *request,
                                    ::glrpc::CommandResult *response,
                                    ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        response->set_message("player_id required");
        return;
    }

    // 快速路径校验（仍在 brpc 线程，避免无意义入队）
    std::string err;
    if (!LocalLogicOk(request->gamelogic_instance_id(), &err)) {
        response->set_ok(false);
        response->set_error_code("ERR_WRONG_GAMELOGIC_OWNER");
        response->set_message(err);
        return;
    }
    if (!FenceOk(request->player_id(), request->session_id(), request->fence_token(),
                 request->generation(), &err)) {
        response->set_ok(false);
        response->set_error_code("FENCE_REJECT");
        response->set_message(err);
        return;
    }

    // 异步：释放 ClosureGuard，由串行队列线程完成 done
    auto cmd = std::make_shared<glrpc::ClientCommand>(*request);
    auto *rsp = response;
    auto *closure = done;
    done_guard.release();
    const uint64_t pid = cmd->player_id();
    const bool posted = PlayerSerialQueue::Instance().TryPost(pid, [cmd, rsp, closure]() {
        // MailClaim：真异步 GameDB，不阻塞同 shard 其他玩家
        game::GameRequest greq;
        if (greq.ParseFromString(cmd->payload()) &&
            (greq.has_mail_claim() || greq.has_mail_batch_claim())) {
            std::string err;
            if (!LocalLogicOk(cmd->gamelogic_instance_id(), &err) ||
                !FenceOk(cmd->player_id(), cmd->session_id(), cmd->fence_token(), cmd->generation(),
                         &err)) {
                brpc::ClosureGuard g(closure);
                rsp->Clear();
                rsp->set_ok(false);
                rsp->set_error_code("FENCE_REJECT");
                rsp->set_message(err);
                return;
            }
            PlayerSerialQueue::Instance().MarkAsyncInFlight(cmd->player_id());
            game::GameResponse placeholder;
            auto on_done = [cmd, rsp, closure](game::GameResponse gr) {
                brpc::ClosureGuard g(closure);
                std::string out_frame;
                gr.SerializeToString(&out_frame);
                rsp->Clear();
                rsp->set_ok(gr.ok());
                rsp->set_message(gr.message());
                if (!out_frame.empty())
                    rsp->set_response_frame(out_frame);
                if (gr.ok())
                    CommitClientSeq(cmd->player_id(), cmd->client_seq(), out_frame);
            };
            bool started = false;
            if (greq.has_mail_claim()) {
                started = MailService::Instance().BeginHandleMailClaimAsync(greq.mail_claim(),
                                                                            &placeholder, on_done);
            } else {
                started = MailService::Instance().BeginHandleMailBatchClaimAsync(
                    greq.mail_batch_claim(), &placeholder, on_done);
            }
            if (!started) {
                PlayerSerialQueue::Instance().ClearAsyncInFlight(cmd->player_id());
                brpc::ClosureGuard g(closure);
                std::string out_frame;
                placeholder.SerializeToString(&out_frame);
                rsp->Clear();
                rsp->set_ok(placeholder.ok());
                rsp->set_message(placeholder.message());
                if (!out_frame.empty())
                    rsp->set_response_frame(out_frame);
            }
            return;
        }
        brpc::ClosureGuard g(closure);
        ExecuteDispatch(*cmd, rsp);
    });
    if (!posted) {
        brpc::ClosureGuard g(closure);
        rsp->Clear();
        rsp->set_ok(false);
        rsp->set_error_code("ERR_OVERLOAD");
        rsp->set_message("player serial queue full");
    }
}

void GameLogicServiceImpl::UnbindPlayer(::google::protobuf::RpcController *controller,
                                        const ::glrpc::UnbindPlayerRequest *request,
                                        ::glrpc::UnbindPlayerResponse *response,
                                        ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request) {
        response->set_ok(false);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_bound_mu);
        auto it = g_bound.find(request->player_id());
        if (it != g_bound.end()) {
            if (!request->session_id().empty() && it->second.session_id != request->session_id()) {
                response->set_ok(false);
                response->set_message("session_id mismatch");
                return;
            }
            if (!request->fence_token().empty() && it->second.fence_token != request->fence_token()) {
                response->set_ok(false);
                response->set_message("fence_token rejected");
                return;
            }
        }
        // 未绑定：幂等成功，仍走 LeaveAll 清残留 AOI。
    }
    // 串行队列上收尾，避免与在途 Dispatch 竞态
    const uint64_t pid = request->player_id();
    const std::string reason = request->reason().empty() ? "unbind" : request->reason();
    done_guard.release();
    auto *rsp = response;
    auto *closure = done;
    if (!PlayerSerialQueue::Instance().TryPost(pid, [pid, reason, rsp, closure]() {
            brpc::ClosureGuard g(closure);
            {
                std::lock_guard<std::mutex> lk(g_bound_mu);
                g_bound.erase(pid);
            }
            AoiPushBatch pushes;
            if (reason == "tcp_disconnect") {
                MapRuntime::Instance().Disconnect(pid, &pushes);
            } else if (reason == "transfer_finalize") {
                MapRuntime::Instance().LeaveAll(pid, &pushes);
                MapInstanceRegistry::Instance().RemovePlayerFromAll(pid);
            } else {
                MapRuntime::Instance().LeaveAll(pid, &pushes);
                MapInstanceRegistry::Instance().RemovePlayerFromAll(pid);
#ifdef WEBSERVER_ENABLE_REDIS
                PlacementStore::Instance().ReleaseByPlayer(pid);
#endif
            }
            GameLogic::Instance().EmitAoi(pushes);
            GameLogic::Instance().FlushLastSafe(pid, reason.c_str());
            GameLogic::Instance().FlushBag(pid, reason);
            rsp->set_ok(true);
            rsp->set_message("unbound");
        })) {
        brpc::ClosureGuard g(closure);
        rsp->set_ok(false);
        rsp->set_message("ERR_OVERLOAD: player serial queue full; unbind not applied");
    }
}

void GameLogicServiceImpl::FreezePlayer(::google::protobuf::RpcController *controller,
                                        const ::glrpc::FreezePlayerRequest *request,
                                        ::glrpc::FreezePlayerResponse *response,
                                        ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        return;
    }
    std::string err;
    if (!FenceOk(request->player_id(), request->session_id(), request->fence_token(), 0, &err)) {
        response->set_ok(false);
        response->set_error_code("FENCE_REJECT");
        response->set_message(err);
        return;
    }
    // 串行队列屏障：入队后排在冻结前 Dispatch 之后执行，再拒绝新写
    const uint64_t pid = request->player_id();
    const std::string transfer_id = request->transfer_id();
    done_guard.release();
    auto *rsp = response;
    auto *closure = done;
    if (!PlayerSerialQueue::Instance().TryPost(pid, [pid, transfer_id, rsp, closure]() {
            brpc::ClosureGuard g(closure);
            std::lock_guard<std::mutex> lk(g_bound_mu);
            auto it = g_bound.find(pid);
            if (it == g_bound.end()) {
                rsp->set_ok(false);
                rsp->set_error_code("NOT_BOUND");
                rsp->set_message("player not bound");
                return;
            }
            if (it->second.frozen &&
                (transfer_id.empty() || it->second.transfer_id == transfer_id)) {
                rsp->set_ok(true);
                rsp->set_message("already frozen");
                return;
            }
            it->second.frozen = true;
            if (!transfer_id.empty())
                it->second.transfer_id = transfer_id;
            rsp->set_ok(true);
            rsp->set_message("frozen");
            LOG_INFO << "FreezePlayer ok player_id=" << pid << " transfer=" << transfer_id;
        })) {
        brpc::ClosureGuard g(closure);
        rsp->set_ok(false);
        rsp->set_error_code("ERR_OVERLOAD");
        rsp->set_message("player serial queue full");
    }
}

void GameLogicServiceImpl::ExportPlayerSnapshot(
    ::google::protobuf::RpcController *controller,
    const ::glrpc::ExportPlayerSnapshotRequest *request,
    ::glrpc::ExportPlayerSnapshotResponse *response, ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        return;
    }
    std::string err;
    if (!FenceOk(request->player_id(), request->session_id(), request->fence_token(), 0, &err)) {
        response->set_ok(false);
        response->set_error_code("FENCE_REJECT");
        response->set_message(err);
        return;
    }
    const auto req_copy = *request;
    const uint64_t pid = request->player_id();
    done_guard.release();
    auto *rsp = response;
    auto *closure = done;
    if (!PlayerSerialQueue::Instance().TryPost(pid, [req_copy, rsp, closure]() {
            brpc::ClosureGuard g(closure);
            BoundPlayer bp;
            {
                std::lock_guard<std::mutex> lk(g_bound_mu);
                auto it = g_bound.find(req_copy.player_id());
                if (it == g_bound.end()) {
                    rsp->set_ok(false);
                    rsp->set_error_code("NOT_BOUND");
                    rsp->set_message("player not bound");
                    return;
                }
                const bool reconnect_snap =
                    req_copy.transfer_id().find("reconnect") != std::string::npos;
                if (!it->second.frozen && !reconnect_snap) {
                    rsp->set_ok(false);
                    rsp->set_error_code("NOT_FROZEN");
                    rsp->set_message("export requires freeze");
                    return;
                }
                bp = it->second;
            }
            std::map<uint32_t, uint32_t> bag;
            std::map<uint32_t, int64_t> cds;
            uint64_t asset_ver = 0;
            if (!GameLogic::Instance().ExportRuntimeState(req_copy.player_id(), &bag, &cds,
                                                          &asset_ver)) {
                rsp->set_ok(false);
                rsp->set_error_code("EXPORT_FAILED");
                rsp->set_message("runtime export failed");
                return;
            }
            auto *snap = rsp->mutable_snapshot();
            snap->set_player_id(req_copy.player_id());
            snap->set_session_id(bp.session_id);
            snap->set_fence_token(bp.fence_token);
            snap->set_generation(bp.generation);
            snap->set_source_gamelogic_id(MapInstanceRegistry::Instance().local_instance_id());
            snap->set_target_gamelogic_id(req_copy.target_gamelogic_id());
            snap->set_source_map_instance_id(bp.map_instance_id);
            snap->set_target_map_instance_id(req_copy.target_map_instance_id());
            snap->set_target_owner_epoch(req_copy.target_owner_epoch());
            snap->set_route_version(bp.route_version);
            snap->set_snapshot_version(1);
            snap->set_transfer_id(req_copy.transfer_id());
            auto *st = snap->mutable_state();
            st->set_last_client_seq(bp.last_client_seq);
            st->set_asset_version(asset_ver);
            for (const auto &kv : bag) {
                if (kv.second == 0)
                    continue;
                auto *e = st->add_bag();
                e->set_item_id(kv.first);
                e->set_count(kv.second);
            }
            for (const auto &kv : cds) {
                auto *e = st->add_skill_cds();
                e->set_skill_id(kv.first);
                e->set_cd_until_ms(kv.second);
            }
            snap->set_checksum(ChecksumRuntimeState(*st));
            game::FullStateSnapshotRsp pub;
            if (GameLogic::Instance().BuildFullStateSnapshot(req_copy.player_id(), 0, &pub) &&
                pub.ok()) {
                std::string bytes;
                if (pub.SerializeToString(&bytes))
                    rsp->set_public_full_snapshot(bytes);
            }
            rsp->set_ok(true);
            rsp->set_message("exported");
            LOG_INFO << "ExportPlayerSnapshot player=" << req_copy.player_id()
                     << " transfer=" << req_copy.transfer_id() << " bag=" << st->bag_size()
                     << " client_seq=" << bp.last_client_seq;
        })) {
        brpc::ClosureGuard g(closure);
        rsp->set_ok(false);
        rsp->set_error_code("ERR_OVERLOAD");
        rsp->set_message("player serial queue full");
    }
}

void GameLogicServiceImpl::ImportPlayerSnapshot(
    ::google::protobuf::RpcController *controller,
    const ::glrpc::ImportPlayerSnapshotRequest *request,
    ::glrpc::ImportPlayerSnapshotResponse *response, ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || !request->has_snapshot() || request->snapshot().player_id() == 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        return;
    }
    const auto req_copy = *request;
    const uint64_t pid = request->snapshot().player_id();
    done_guard.release();
    auto *rsp = response;
    auto *closure = done;
    if (!PlayerSerialQueue::Instance().TryPost(pid, [req_copy, rsp, closure]() {
            brpc::ClosureGuard g(closure);
            const auto &snap = req_copy.snapshot();
            if (!snap.target_gamelogic_id().empty() &&
                snap.target_gamelogic_id() != MapInstanceRegistry::Instance().local_instance_id()) {
                rsp->set_ok(false);
                rsp->set_error_code("ERR_WRONG_GAMELOGIC_OWNER");
                rsp->set_message("import target mismatch");
                return;
            }
            const std::string expect = ChecksumRuntimeState(snap.state());
            if (!snap.checksum().empty() && snap.checksum() != expect) {
                rsp->set_ok(false);
                rsp->set_error_code("CHECKSUM_MISMATCH");
                rsp->set_message("snapshot checksum mismatch");
                return;
            }
            {
                std::lock_guard<std::mutex> lk(g_bound_mu);
                auto it = g_bound.find(snap.player_id());
                if (it == g_bound.end()) {
                    rsp->set_ok(false);
                    rsp->set_error_code("NOT_BOUND");
                    rsp->set_message("import requires Bind/Prepare first");
                    return;
                }
                if (!it->second.last_import_checksum.empty() &&
                    it->second.last_import_checksum == snap.checksum()) {
                    rsp->set_ok(true);
                    rsp->set_already_applied(true);
                    rsp->set_message("import idempotent");
                    return;
                }
            }
            std::map<uint32_t, uint32_t> bag;
            std::map<uint32_t, int64_t> cds;
            for (const auto &e : snap.state().bag())
                bag[e.item_id()] = e.count();
            for (const auto &e : snap.state().skill_cds())
                cds[e.skill_id()] = e.cd_until_ms();
            if (!GameLogic::Instance().ImportRuntimeState(snap.player_id(), bag, cds,
                                                          snap.state().asset_version())) {
                rsp->set_ok(false);
                rsp->set_error_code("IMPORT_FAILED");
                return;
            }
            {
                std::lock_guard<std::mutex> lk(g_bound_mu);
                auto it = g_bound.find(snap.player_id());
                if (it != g_bound.end()) {
                    it->second.last_client_seq = snap.state().last_client_seq();
                    it->second.last_import_checksum =
                        snap.checksum().empty() ? expect : snap.checksum();
                    it->second.frozen = false;
                    if (!snap.transfer_id().empty())
                        it->second.transfer_id = snap.transfer_id();
                    if (snap.target_map_instance_id() != 0)
                        it->second.map_instance_id = snap.target_map_instance_id();
                    if (snap.target_owner_epoch() != 0)
                        it->second.map_owner_epoch = snap.target_owner_epoch();
                }
            }
            rsp->set_ok(true);
            rsp->set_already_applied(false);
            rsp->set_message("imported");
            LOG_INFO << "ImportPlayerSnapshot player=" << snap.player_id()
                     << " transfer=" << snap.transfer_id()
                     << " client_seq=" << snap.state().last_client_seq();
        })) {
        brpc::ClosureGuard g(closure);
        rsp->set_ok(false);
        rsp->set_error_code("ERR_OVERLOAD");
        rsp->set_message("player serial queue full");
    }
}
