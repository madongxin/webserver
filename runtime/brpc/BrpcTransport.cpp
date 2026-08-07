#include "BrpcTransport.h"

#include "BrpcChannelManager.h"
#include "GameRequestTransport.h"
#include "Logging.h"
#include "MapPlacement.h"
#include "MessageRoute.h"
#include "ProtoFraming.h"
#include "forward.pb.h"
#include "game.pb.h"

#include <brpc/channel.h>

#include <memory>

namespace {

std::string BuildErrorFrame(const std::string &request_payload, const std::string &message) {
    game::GameRequest req;
    game::GameResponse rsp;
    if (req.ParseFromString(request_payload))
        rsp.set_seq(req.seq());
    rsp.set_ok(false);
    rsp.set_message(message);
    std::string body;
    if (!rsp.SerializeToString(&body))
        return {};
    std::string frame;
    if (!gameproto::EncodeFrame(body, &frame))
        return {};
    return frame;
}

struct ForwardCallContext {
    brpc::Controller cntl;
    fwd::ForwardReq req;
    fwd::ForwardRsp rsp;
    std::shared_ptr<ReplySink> sink;
    std::string request_payload;
    bool to_world = false;
};

void OnForwardDone(ForwardCallContext *ctx) {
    std::unique_ptr<ForwardCallContext> guard(ctx);
    if (!ctx->sink)
        return;
    if (ctx->cntl.Failed()) {
        LOG_ERROR << "BrpcTransport Forward failed to_world=" << ctx->to_world << ": "
                  << ctx->cntl.ErrorText();
        const std::string err =
            BuildErrorFrame(ctx->request_payload, std::string("rpc_failed: ") + ctx->cntl.ErrorText());
        if (!err.empty())
            ctx->sink->SendFrame(err);
        return;
    }
    if (!ctx->rsp.ok() || ctx->rsp.response_frame().empty()) {
        const std::string msg =
            ctx->rsp.message().empty() ? "empty_response" : ctx->rsp.message();
        const std::string err = BuildErrorFrame(ctx->request_payload, msg);
        if (!err.empty())
            ctx->sink->SendFrame(err);
        return;
    }
    ctx->sink->SendFrame(ctx->rsp.response_frame());
}

bool FillMapRouteFromRequest(const game::GameRequest &req, SessionHandle *handle,
                             MapPlacementRecord *placement) {
    if (!handle || !placement)
        return false;
    if (req.body_case() == game::GameRequest::kEnterMap) {
        const auto &e = req.enter_map();
        if (!MapPlacement::Instance().ResolveOrAllocate(e.realm_id(), e.map_template_id(),
                                                       e.map_instance_id(), placement))
            return false;
        handle->map_instance_id = placement->map_instance_id;
        handle->owner_epoch = placement->owner_epoch;
        handle->route_version = placement->route_version;
        handle->gamelogic_instance_id = placement->owner_gamelogic_id;
        return true;
    }
    uint64_t mid = 0;
    if (req.body_case() == game::GameRequest::kLeaveMap)
        mid = req.leave_map().map_instance_id();
    else if (req.body_case() == game::GameRequest::kMapPing)
        mid = req.map_ping().map_instance_id();
    if (mid == 0)
        return false;
    if (!MapPlacement::Instance().Get(mid, placement))
        return false;
    handle->map_instance_id = placement->map_instance_id;
    handle->owner_epoch = placement->owner_epoch;
    handle->route_version = placement->route_version;
    handle->gamelogic_instance_id = placement->owner_gamelogic_id;
    return true;
}

bool InitWorldChannel(std::unique_ptr<brpc::Channel> *out, const std::string &addr, int timeout_ms) {
    if (!out || addr.empty())
        return false;
    auto ch = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions opt;
    opt.protocol = "baidu_std";
    opt.timeout_ms = timeout_ms > 0 ? timeout_ms : 3000;
    opt.max_retry = 0;
    if (ch->Init(addr.c_str(), &opt) != 0) {
        LOG_ERROR << "BrpcTransport: World Channel::Init failed addr=" << addr;
        return false;
    }
    LOG_INFO << "BrpcTransport: world channel ready addr=" << addr;
    *out = std::move(ch);
    return true;
}

}  // namespace

BrpcTransport &BrpcTransport::Instance() {
    static BrpcTransport g;
    return g;
}

bool BrpcTransport::EnsureStarted(const std::vector<std::string> &logic_addrs, int timeout_ms) {
    return EnsureStarted(logic_addrs, {}, {}, timeout_ms);
}

bool BrpcTransport::EnsureStarted(const std::vector<std::string> &logic_addrs,
                                  const std::vector<std::string> &logic_instance_ids,
                                  int timeout_ms) {
    return EnsureStarted(logic_addrs, logic_instance_ids, {}, timeout_ms);
}

bool BrpcTransport::EnsureStarted(const std::vector<std::string> &logic_addrs,
                                  const std::vector<std::string> &logic_instance_ids,
                                  const std::vector<std::string> &world_addrs, int timeout_ms) {
    if (!BrpcChannelManager::Instance().Init(logic_addrs, logic_instance_ids, timeout_ms))
        return false;
    MapPlacement::Instance().ConfigureOwners(BrpcChannelManager::Instance().instance_ids());
    world_channel_.reset();
    if (!world_addrs.empty()) {
        if (!InitWorldChannel(&world_channel_, world_addrs.front(), timeout_ms))
            return false;
    } else {
        LOG_WARN << "BrpcTransport: world_addrs empty; mail/chat will fail on gateway role";
    }
    GameRequestTransport::Set(this);
    started_ = true;
    LOG_INFO << "BrpcTransport started logic=" << logic_addrs.size()
             << " world=" << (world_channel_ ? 1 : 0);
    return true;
}

void BrpcTransport::PostPlayerRequest(const SessionHandle &handle,
                                      std::string request_payload,
                                      std::shared_ptr<ReplySink> sink) {
    SessionHandle route = handle;
    MapPlacementRecord placement;
    game::GameRequest parsed;
    const bool parsed_ok = parsed.ParseFromString(request_payload);
    const bool to_world = parsed_ok && gameproto::IsWorldBoundRequest(parsed);

    if (parsed_ok && !to_world) {
        if (parsed.body_case() == game::GameRequest::kEnterMap ||
            parsed.body_case() == game::GameRequest::kLeaveMap ||
            parsed.body_case() == game::GameRequest::kMapPing) {
            if (!FillMapRouteFromRequest(parsed, &route, &placement)) {
                if (sink) {
                    const std::string err = BuildErrorFrame(request_payload, "map_placement_failed");
                    if (!err.empty())
                        sink->SendFrame(err);
                }
                return;
            }
        }
    }

    brpc::Channel *ch = nullptr;
    if (to_world) {
        ch = world_channel_.get();
        if (!ch) {
            if (sink) {
                const std::string err = BuildErrorFrame(request_payload, "no_world_channel");
                if (!err.empty())
                    sink->SendFrame(err);
            }
            return;
        }
    } else {
        const bool is_register =
            parsed_ok && parsed.body_case() == game::GameRequest::kRegister;
        // 会话生命周期内必须粘性到 Session 返回的 gamelogic_instance_id（禁止随机轮询）
        // Register 尚未 AcquireSession：允许按 player_id%N 选 Logic（仅分配账号）
        if (route.gamelogic_instance_id.empty() && !is_register && route.player_id != 0) {
            LOG_ERROR << "BrpcTransport: missing sticky gamelogic_instance_id player_id="
                      << route.player_id;
            if (sink) {
                const std::string err =
                    BuildErrorFrame(request_payload, "missing_gamelogic_route");
                if (!err.empty())
                    sink->SendFrame(err);
            }
            return;
        }
        if (!route.gamelogic_instance_id.empty())
            ch = BrpcChannelManager::Instance().ChannelForInstance(route.gamelogic_instance_id);
        if (!ch && is_register)
            ch = BrpcChannelManager::Instance().ChannelForPlayer(route.player_id);
        if (!ch) {
            LOG_ERROR << "BrpcTransport: no logic channel instance=" << route.gamelogic_instance_id
                      << " player_id=" << route.player_id;
            if (sink) {
                const std::string err = BuildErrorFrame(request_payload, "no_logic_channel");
                if (!err.empty())
                    sink->SendFrame(err);
            }
            return;
        }
    }

    auto *ctx = new ForwardCallContext();
    ctx->sink = std::move(sink);
    ctx->request_payload = request_payload;
    ctx->to_world = to_world;
    ctx->req.mutable_meta()->set_player_id(route.player_id);
    ctx->req.mutable_meta()->set_connection_id(route.connection_id);
    ctx->req.mutable_meta()->set_generation(route.generation);
    // 可信路由字段由 Gateway 填写，不信任客户端
    if (!to_world) {
        ctx->req.mutable_meta()->set_map_instance_id(route.map_instance_id);
        ctx->req.mutable_meta()->set_owner_epoch(route.owner_epoch);
        ctx->req.mutable_meta()->set_route_version(route.route_version);
        ctx->req.mutable_meta()->set_gamelogic_instance_id(route.gamelogic_instance_id);
        ctx->req.mutable_meta()->set_session_id(route.session_id);
        ctx->req.mutable_meta()->set_fence_token(route.fence_token);
    }
    ctx->req.set_request_payload(std::move(request_payload));

    if (to_world) {
        fwd::WorldForward_Stub stub(ch);
        stub.Forward(&ctx->cntl, &ctx->req, &ctx->rsp, brpc::NewCallback(&OnForwardDone, ctx));
    } else {
        fwd::GameLogicForward_Stub stub(ch);
        stub.Forward(&ctx->cntl, &ctx->req, &ctx->rsp, brpc::NewCallback(&OnForwardDone, ctx));
    }
}
