#include "GameLogicServiceImpl.h"

#include "ForwardMetaContext.h"
#include "GameLogic.h"
#include "GameService.h"
#include "Logging.h"
#include "MapInstanceRegistry.h"
#include "game.pb.h"

#include <brpc/controller.h>

#include <mutex>
#include <unordered_map>

namespace {

struct BoundPlayer {
    std::string session_id;
    std::string fence_token;
    std::string gateway_instance_id;
    uint64_t map_instance_id = 0;
    uint64_t map_owner_epoch = 0;
    uint64_t route_version = 0;
    uint64_t generation = 0;
    bool frozen = false;
    std::string transfer_id;
};

std::mutex g_bound_mu;
std::unordered_map<uint64_t, BoundPlayer> g_bound;

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
    bp.frozen = false;  // Prepare/Bind 后可写
    // Bind 幂等：同 session+fence 重复 Bind 覆盖 gateway 路由；更高 generation 顶替旧绑定
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
        // 同 transfer_id 重复 Prepare 幂等
        if (it != g_bound.end() && !bp.transfer_id.empty() &&
            it->second.transfer_id == bp.transfer_id && !it->second.frozen) {
            response->set_ok(true);
            response->set_message("prepare idempotent");
            response->set_bag_item_kinds(0);
            return;
        }
        g_bound[player_id] = bp;
    }
    GameLogic::Instance().BindAuthenticatedPlayer(player_id);
    response->set_ok(true);
    response->set_message("player ready");
    response->set_bag_item_kinds(0);
    LOG_INFO << "BindPlayer ok player_id=" << player_id << " session=" << request->session_id()
             << " gen=" << request->generation() << " gw=" << request->gateway_instance_id();
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
    std::string err;
    if (!FenceOk(request->player_id(), request->session_id(), request->fence_token(),
                 request->generation(), &err)) {
        response->set_ok(false);
        response->set_error_code("FENCE_REJECT");
        response->set_message(err);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_bound_mu);
        auto it = g_bound.find(request->player_id());
        if (it != g_bound.end()) {
            if (it->second.frozen) {
                response->set_ok(false);
                response->set_error_code("PLAYER_FROZEN");
                response->set_message("player frozen for transfer");
                return;
            }
            if (request->route_version() != 0 && it->second.route_version != 0 &&
                request->route_version() < it->second.route_version) {
                response->set_ok(false);
                response->set_error_code("ERR_ROUTE_STALE");
                response->set_message("route_version stale");
                return;
            }
        }
    }
    if (request->map_instance_id() != 0) {
        const MapWriteFence fence = MapInstanceRegistry::Instance().CheckWrite(
            request->map_instance_id(), request->map_owner_epoch());
        // EnterMap 首次到达本 Logic 时尚无 Claim：放行由 HandleEnterMap 完成 Claim
        const bool allow_first_claim =
            fence == MapWriteFence::NotClaimed &&
            (request->message_type() == "enter_map" || request->message_type().empty());
        if (fence != MapWriteFence::Ok && !allow_first_claim) {
            response->set_ok(false);
            if (fence == MapWriteFence::LeaseExpired) {
                response->set_error_code("LEASE_EXPIRED");
                response->set_message("map owner lease expired");
                MapInstanceRegistry::Instance().Release(request->map_instance_id());
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
    meta.player_id = request->player_id();
    meta.map_instance_id = request->map_instance_id();
    meta.owner_epoch = request->map_owner_epoch();
    meta.route_version = request->route_version();
    meta.gamelogic_instance_id = request->gamelogic_instance_id();
    meta.session_id = request->session_id();
    meta.fence_token = request->fence_token();
    meta.generation = request->generation();
    ForwardMetaContext::Set(meta);

    std::string out_frame;
    const bool ok = gameproto::HandleFrame(request->payload(), &out_frame);
    ForwardMetaContext::Clear();
    response->set_ok(ok);
    response->set_message(ok ? "ok" : "handle_frame_failed");
    if (!out_frame.empty())
        response->set_response_frame(out_frame);
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
    std::string err;
    if (!request->fence_token().empty() &&
        !FenceOk(request->player_id(), request->session_id(), request->fence_token(), 0, &err)) {
        response->set_ok(false);
        response->set_message(err);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_bound_mu);
        g_bound.erase(request->player_id());
    }
    GameLogic::Instance().FlushBag(request->player_id(),
                                    request->reason().empty() ? "unbind" : request->reason());
    response->set_ok(true);
    response->set_message("unbound");
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
    std::lock_guard<std::mutex> lk(g_bound_mu);
    auto it = g_bound.find(request->player_id());
    if (it == g_bound.end()) {
        response->set_ok(false);
        response->set_error_code("NOT_BOUND");
        response->set_message("player not bound");
        return;
    }
    // 幂等：同 transfer_id 已冻结
    if (it->second.frozen &&
        (request->transfer_id().empty() || it->second.transfer_id == request->transfer_id())) {
        response->set_ok(true);
        response->set_message("already frozen");
        return;
    }
    it->second.frozen = true;
    if (!request->transfer_id().empty())
        it->second.transfer_id = request->transfer_id();
    response->set_ok(true);
    response->set_message("frozen");
    LOG_INFO << "FreezePlayer ok player_id=" << request->player_id()
             << " transfer=" << request->transfer_id();
}
