#include "BrpcTransport.h"

#include "BrpcChannelManager.h"
#include "GameRequestTransport.h"
#include "Logging.h"
#include "MapPlacement.h"
#include "MessageRoute.h"
#include "PlacementStore.h"
#include "ProtoFraming.h"
#include "SessionRpcClient.h"
#include "forward.pb.h"
#include "gamelogic_rpc.pb.h"
#include "game.pb.h"
#include "session.pb.h"

#include <brpc/channel.h>

#include <atomic>
#include <memory>
#include <sstream>

namespace {

std::atomic<uint64_t> g_dispatch_req_id{1};

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

struct DispatchCallContext {
    brpc::Controller cntl;
    glrpc::ClientCommand req;
    glrpc::CommandResult rsp;
    std::shared_ptr<ReplySink> sink;
    std::string request_payload;
};

struct WorldForwardCallContext {
    brpc::Controller cntl;
    fwd::ForwardReq req;
    fwd::ForwardRsp rsp;
    std::shared_ptr<ReplySink> sink;
    std::string request_payload;
};

void OnDispatchDone(DispatchCallContext *ctx) {
    std::unique_ptr<DispatchCallContext> guard(ctx);
    if (!ctx->sink)
        return;
    if (ctx->cntl.Failed()) {
        LOG_ERROR << "BrpcTransport Dispatch failed: " << ctx->cntl.ErrorText();
        const std::string err =
            BuildErrorFrame(ctx->request_payload, std::string("rpc_failed: ") + ctx->cntl.ErrorText());
        if (!err.empty())
            ctx->sink->SendFrame(err);
        return;
    }
    if (!ctx->rsp.ok() || ctx->rsp.response_frame().empty()) {
        const std::string msg =
            ctx->rsp.message().empty() ? (ctx->rsp.error_code().empty() ? "dispatch_failed"
                                                                        : ctx->rsp.error_code())
                                       : ctx->rsp.message();
        const std::string err = BuildErrorFrame(ctx->request_payload, msg);
        if (!err.empty())
            ctx->sink->SendFrame(err);
        return;
    }
    ctx->sink->SendFrame(ctx->rsp.response_frame());
}

void OnWorldForwardDone(WorldForwardCallContext *ctx) {
    std::unique_ptr<WorldForwardCallContext> guard(ctx);
    if (!ctx->sink)
        return;
    if (ctx->cntl.Failed()) {
        LOG_ERROR << "BrpcTransport WorldForward failed: " << ctx->cntl.ErrorText();
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

void ApplyPlacementToHandle(const MapPlacementRecord &p, SessionHandle *handle) {
    handle->map_instance_id = p.map_instance_id;
    handle->owner_epoch = p.owner_epoch;
    handle->route_version = p.route_version;
    handle->gamelogic_instance_id = p.owner_gamelogic_id;
}

bool PlacementFromPb(const sess::PlacementRecord &pb, MapPlacementRecord *out) {
    if (!out || pb.map_instance_id() == 0)
        return false;
    if (pb.state() == "RECOVERING" || pb.state() == "CLOSED" || pb.state() == "FROZEN")
        return false;
    out->realm_id = pb.realm_id();
    out->map_template_id = pb.map_template_id();
    out->map_instance_id = pb.map_instance_id();
    out->owner_gamelogic_id = pb.owner_logic_server_id();
    out->owner_epoch = pb.owner_epoch();
    out->route_version = pb.route_version();
    out->frozen = (pb.state() == "FROZEN" || pb.state() == "MIGRATING");
    MapPlacement::Instance().UpsertCache(*out);
    return !out->owner_gamelogic_id.empty();
}

bool ResolvePlacementAuthority(uint32_t realm_id, uint64_t map_template_id, uint64_t map_instance_id,
                               MapPlacementRecord *out) {
    if (!out)
        return false;
    if (SessionRpcClient::Instance().ready()) {
        if (map_instance_id != 0) {
            sess::GetPlacementResponse grsp;
            if (!SessionRpcClient::Instance().GetPlacement(map_instance_id, &grsp))
                return false;
            return PlacementFromPb(grsp.placement(), out);
        }
        sess::ResolveOrCreateMapRequest req;
        req.set_realm_id(realm_id);
        req.set_map_template_id(map_template_id);
        req.set_map_instance_id(0);
        sess::ResolveOrCreateMapResponse rsp;
        if (!SessionRpcClient::Instance().ResolveOrCreateMap(req, &rsp))
            return false;
        return PlacementFromPb(rsp.placement(), out);
    }
    if (PlacementStore::Instance().Available()) {
        if (map_instance_id != 0) {
            PlacementRecord rec;
            if (!PlacementStore::Instance().Get(map_instance_id, &rec))
                return false;
            if (rec.state == PlacementState::Recovering || rec.state == PlacementState::Closed ||
                rec.state == PlacementState::Frozen)
                return false;
            out->realm_id = rec.realm_id;
            out->map_template_id = rec.map_template_id;
            out->map_instance_id = rec.map_instance_id;
            out->owner_gamelogic_id = rec.owner_logic_server_id;
            out->owner_epoch = rec.owner_epoch;
            out->route_version = rec.route_version;
            MapPlacement::Instance().UpsertCache(*out);
            return true;
        }
        ResolveOrCreateInput in;
        in.realm_id = realm_id;
        in.map_template_id = map_template_id;
        ResolveOrCreateResult result;
        if (!PlacementStore::Instance().ResolveOrCreate(in, &result) || !result.ok)
            return false;
        out->realm_id = result.placement.realm_id;
        out->map_template_id = result.placement.map_template_id;
        out->map_instance_id = result.placement.map_instance_id;
        out->owner_gamelogic_id = result.placement.owner_logic_server_id;
        out->owner_epoch = result.placement.owner_epoch;
        out->route_version = result.placement.route_version;
        MapPlacement::Instance().UpsertCache(*out);
        return true;
    }
    // 开发回退：进程内缓存（非权威）
    return MapPlacement::Instance().ResolveOrAllocate(realm_id, map_template_id, map_instance_id, out);
}

bool FillMapRouteFromRequest(const game::GameRequest &req, SessionHandle *handle,
                             MapPlacementRecord *placement) {
    if (!handle || !placement)
        return false;
    if (req.body_case() == game::GameRequest::kEnterMap) {
        const auto &e = req.enter_map();
        if (!ResolvePlacementAuthority(e.realm_id(), e.map_template_id(), e.map_instance_id(),
                                       placement))
            return false;
        ApplyPlacementToHandle(*placement, handle);
        return true;
    }
    uint64_t mid = 0;
    if (req.body_case() == game::GameRequest::kLeaveMap)
        mid = req.leave_map().map_instance_id();
    else if (req.body_case() == game::GameRequest::kMapPing)
        mid = req.map_ping().map_instance_id();
    if (mid == 0)
        return false;
    if (ResolvePlacementAuthority(0, 0, mid, placement)) {
        ApplyPlacementToHandle(*placement, handle);
        return true;
    }
    if (!MapPlacement::Instance().Get(mid, placement))
        return false;
    ApplyPlacementToHandle(*placement, handle);
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
    LOG_INFO << "BrpcTransport started (Dispatch path) logic=" << logic_addrs.size()
             << " world=" << (world_channel_ ? 1 : 0);
    return true;
}

void BrpcTransport::PostPlayerRequest(const SessionHandle &handle, std::string request_payload,
                                      std::shared_ptr<ReplySink> sink) {
    SessionHandle route = handle;
    MapPlacementRecord placement;
    game::GameRequest parsed;
    const bool parsed_ok = parsed.ParseFromString(request_payload);
    const bool to_world = parsed_ok && gameproto::IsWorldBoundRequest(parsed);

    // EnterMap 由 GatewayEnterMapOrchestrator 在 worker 编排；此处禁止 Placement 直改 sticky 后裸 Dispatch
    if (parsed_ok && !to_world) {
        if (parsed.body_case() == game::GameRequest::kEnterMap) {
            if (sink) {
                const std::string err =
                    BuildErrorFrame(request_payload, "enter_map_must_use_gateway_orchestrator");
                if (!err.empty())
                    sink->SendFrame(err);
            }
            return;
        }
        if (parsed.body_case() == game::GameRequest::kLeaveMap ||
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

    if (to_world) {
        brpc::Channel *ch = world_channel_.get();
        if (!ch) {
            if (sink) {
                const std::string err = BuildErrorFrame(request_payload, "no_world_channel");
                if (!err.empty())
                    sink->SendFrame(err);
            }
            return;
        }
        auto *ctx = new WorldForwardCallContext();
        ctx->sink = std::move(sink);
        ctx->request_payload = request_payload;
        ctx->req.mutable_meta()->set_player_id(route.player_id);
        ctx->req.mutable_meta()->set_connection_id(route.connection_id);
        ctx->req.mutable_meta()->set_generation(route.generation);
        ctx->req.set_request_payload(std::move(request_payload));
        fwd::WorldForward_Stub stub(ch);
        stub.Forward(&ctx->cntl, &ctx->req, &ctx->rsp, brpc::NewCallback(&OnWorldForwardDone, ctx));
        return;
    }

    // 正式业务：异步 Dispatch（不再走 GameLogicForward）
    const bool is_register = parsed_ok && parsed.body_case() == game::GameRequest::kRegister;
    if (route.gamelogic_instance_id.empty() && !is_register) {
        LOG_ERROR << "BrpcTransport: missing sticky gamelogic_instance_id player_id="
                  << route.player_id;
        if (sink) {
            const std::string err = BuildErrorFrame(request_payload, "unauthenticated_or_no_route");
            if (!err.empty())
                sink->SendFrame(err);
        }
        return;
    }

    brpc::Channel *ch = nullptr;
    if (!route.gamelogic_instance_id.empty())
        ch = BrpcChannelManager::Instance().ChannelForInstance(route.gamelogic_instance_id);
    if (!ch && is_register)
        ch = BrpcChannelManager::Instance().ChannelForPlayer(route.player_id);
    if (!ch) {
        LOG_ERROR << "BrpcTransport: unknown logic id fail-closed instance="
                  << route.gamelogic_instance_id << " player_id=" << route.player_id;
        if (sink) {
            const std::string err = BuildErrorFrame(request_payload, "unknown_logic_server_id");
            if (!err.empty())
                sink->SendFrame(err);
        }
        return;
    }

    auto *ctx = new DispatchCallContext();
    ctx->sink = std::move(sink);
    ctx->request_payload = request_payload;
    const uint64_t req_id = g_dispatch_req_id.fetch_add(1);
    ctx->req.set_request_id(req_id);
    ctx->req.set_player_id(route.player_id);
    ctx->req.set_session_id(route.session_id);
    ctx->req.set_fence_token(route.fence_token);
    ctx->req.set_gamelogic_instance_id(route.gamelogic_instance_id);
    ctx->req.set_map_instance_id(route.map_instance_id);
    ctx->req.set_map_owner_epoch(route.owner_epoch);
    ctx->req.set_route_version(route.route_version);
    ctx->req.set_generation(route.generation);
    ctx->req.set_client_seq(parsed_ok ? parsed.seq() : 0);
    if (parsed_ok) {
        std::ostringstream os;
        os << "body_" << static_cast<int>(parsed.body_case());
        ctx->req.set_message_type(os.str());
    }
    std::ostringstream tid;
    tid << "gw-" << route.connection_id << "-" << req_id;
    ctx->req.set_trace_id(tid.str());
    ctx->req.set_trace_context(tid.str());
    ctx->req.set_deadline_ms(3000);
    ctx->req.set_payload(std::move(request_payload));
    ctx->cntl.set_timeout_ms(3000);

    glrpc::GameLogicService_Stub stub(ch);
    stub.Dispatch(&ctx->cntl, &ctx->req, &ctx->rsp, brpc::NewCallback(&OnDispatchDone, ctx));
}
