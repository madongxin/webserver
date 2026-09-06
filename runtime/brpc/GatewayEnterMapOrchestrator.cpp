#include "GatewayEnterMapOrchestrator.h"

#include "GatewayAuthClients.h"
#include "GatewayIdentity.h"
#include "Logging.h"
#include "MapCatalog.h"
#include "MapLineView.h"
#include "PlacementStore.h"
#include "PlayerSerialQueue.h"
#include "ProtoFraming.h"
#include "PublicError.h"
#include "RpcOffloadPool.h"
#include "SceneKind.h"
#include "SessionStore.h"
#include "game.pb.h"
#include "session.pb.h"

#include <memory>
#include <utility>

namespace gameproto {

namespace {

bool EncodeErr(const game::GameRequest &req, const std::string &msg, std::string *frame,
               const std::string &error_code = {}) {
    game::GameResponse rsp;
    rsp.set_seq(req.seq());
    rsp.set_ok(false);
    rsp.set_message(msg);
    if (!error_code.empty())
        rsp.set_error_code(error_code);
    auto *body = rsp.mutable_enter_map();
    body->set_ok(false);
    body->set_message(msg);
    if (!error_code.empty())
        body->set_error_code(error_code);
    gameproto::PromotePublicError(&rsp, 0);
    std::string raw;
    return rsp.SerializeToString(&raw) && EncodeFrame(raw, frame);
}

void ApplyEnterMapScenePolicy(const game::EnterMapReq &e, sess::ResolveOrCreateMapRequest *req,
                              ResolveOrCreateInput *in) {
    MapScenePolicy pol;
    const bool have = MapCatalog::Instance().GetScenePolicy(e.map_template_id(), &pol);
    const std::string kind = have ? SceneKindToString(pol.kind) : std::string();
    if (req) {
        req->set_line_no(e.line_no());
        if (!kind.empty())
            req->set_kind(kind);
        if (have && pol.kind == SceneKind::Line) {
            req->set_soft_cap(pol.soft_cap);
            req->set_hard_cap(pol.hard_cap);
            req->set_max_lines(pol.max_lines);
        } else if (have && pol.kind == SceneKind::Dungeon) {
            req->set_soft_cap(pol.soft_cap);
            req->set_hard_cap(pol.hard_cap);
        }
    }
    if (in) {
        in->line_no = e.line_no();
        in->kind = kind;
        if (have && pol.kind == SceneKind::Line) {
            in->soft_cap = pol.soft_cap;
            in->hard_cap = pol.hard_cap;
            in->max_lines = pol.max_lines;
            in->min_lines = pol.min_lines;
            in->empty_close_delay = pol.empty_close_delay;
        } else if (have && pol.kind == SceneKind::Dungeon) {
            in->soft_cap = pol.soft_cap;
            in->hard_cap = pol.hard_cap;
            in->empty_close_delay = pol.empty_close_delay;
        }
    }
}

bool ResolveTarget(const game::EnterMapReq &e, uint64_t player_id, std::string *owner,
                   uint64_t *map_id, uint64_t *epoch, uint64_t *placement_rv, std::string *err,
                   std::string *err_code) {
    if (GatewayAuthClients::Instance().ready()) {
        sess::ResolveOrCreateMapRequest req;
        req.set_realm_id(e.realm_id());
        req.set_map_template_id(e.map_template_id());
        req.set_map_instance_id(e.map_instance_id());
        req.set_player_id(player_id != 0 ? player_id : e.player_id());
        req.set_operation_id(e.operation_id());
        req.set_public_map_capacity(0);
        req.set_queue_token(e.queue_token());
        ApplyEnterMapScenePolicy(e, &req, nullptr);
        sess::ResolveOrCreateMapResponse rsp;
        if (!GatewayAuthClients::Instance().ResolveOrCreateMap(req, &rsp) || !rsp.ok()) {
            if (err)
                *err = rsp.message().empty() ? "resolve map failed" : rsp.message();
            if (err_code)
                *err_code = rsp.error_code();
            return false;
        }
        const auto &p = rsp.placement();
        if (p.state() == "RECOVERING" || p.state() == "CLOSED" || p.state() == "FROZEN" ||
            p.state() == "DRAINING" || p.state() == "MIGRATING") {
            if (err)
                *err = "placement not ready";
            return false;
        }
        *owner = p.owner_logic_server_id();
        *map_id = p.map_instance_id();
        *epoch = p.owner_epoch();
        *placement_rv = p.route_version();
        return !owner->empty() && *map_id != 0;
    }
    if (PlacementStore::Instance().Available()) {
        ResolveOrCreateInput in;
        in.realm_id = e.realm_id();
        in.map_template_id = e.map_template_id();
        in.map_instance_id = e.map_instance_id();
        in.player_id = player_id != 0 ? player_id : e.player_id();
        in.operation_id = e.operation_id();
        in.queue_token = e.queue_token();
        ApplyEnterMapScenePolicy(e, nullptr, &in);
        ResolveOrCreateResult result;
        if (!PlacementStore::Instance().ResolveOrCreate(in, &result) || !result.ok) {
            if (err)
                *err = result.message.empty() ? "placement resolve failed" : result.message;
            if (err_code)
                *err_code = result.error_code;
            return false;
        }
        *owner = result.placement.owner_logic_server_id;
        *map_id = result.placement.map_instance_id;
        *epoch = result.placement.owner_epoch;
        *placement_rv = result.placement.route_version;
        return !owner->empty();
    }
    if (err)
        *err = "no placement authority";
    return false;
}

void AbortTransfer(uint64_t player_id, const std::string &fence, const std::string &tid,
                   const std::string &from_logic) {
    if (tid.empty())
        return;
    if (GatewayAuthClients::Instance().ready()) {
        sess::AbortPlayerTransferRequest areq;
        areq.set_player_id(player_id);
        areq.set_fence_token(fence);
        areq.set_transfer_id(tid);
        areq.set_reason("enter_map_failed");
        sess::AbortPlayerTransferResponse arsp;
        GatewayAuthClients::Instance().AbortPlayerTransfer(areq, &arsp);
    } else if (SessionStore::Instance().Available()) {
        std::string err;
        SessionStore::Instance().AbortPlayerTransfer(player_id, fence, tid, &err, nullptr);
    }
    (void)from_logic;
}

}  // namespace

bool OrchestrateGatewayEnterMap(const SessionHandle &sticky, const std::string &request_payload,
                                std::string *response_frame, SessionHandle *route_out) {
    if (route_out)
        *route_out = sticky;
    game::GameRequest req;
    if (!req.ParseFromString(request_payload) || !req.has_enter_map()) {
        game::GameResponse rsp;
        rsp.set_ok(false);
        rsp.set_message("invalid enter_map");
        std::string raw;
        return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
    }
    if (sticky.player_id == 0 || sticky.fence_token.empty() ||
        sticky.gamelogic_instance_id.empty()) {
        return EncodeErr(req, "unauthenticated_or_no_route", response_frame);
    }

    std::string target_logic;
    uint64_t map_id = 0, epoch = 0, placement_rv = 0;
    std::string err;
    std::string err_code;
    if (!ResolveTarget(req.enter_map(), sticky.player_id, &target_logic, &map_id, &epoch,
                       &placement_rv, &err, &err_code))
        return EncodeErr(req, err, response_frame, err_code);

    const std::string &from_logic = sticky.gamelogic_instance_id;
    const bool cross = (target_logic != from_logic);
    const std::string gw_id =
        GatewayIdentity::Instance().ready() ? GatewayIdentity::Instance().id() : std::string();

    std::string transfer_id;
    if (cross) {
        const bool sess_rpc = GatewayAuthClients::Instance().ready();
        const bool sess_local = SessionStore::Instance().Available();
        if (!sess_rpc && !sess_local)
            return EncodeErr(req, "session not ready for transfer", response_frame);

        if (sess_rpc) {
            sess::BeginPlayerTransferRequest breq;
            breq.set_player_id(sticky.player_id);
            breq.set_fence_token(sticky.fence_token);
            breq.set_expected_route_version(sticky.route_version);
            breq.set_from_gamelogic_instance_id(from_logic);
            breq.set_to_gamelogic_instance_id(target_logic);
            breq.set_map_instance_id(map_id);
            breq.set_map_owner_epoch(epoch);
            breq.set_gateway_instance_id(gw_id);
            sess::BeginPlayerTransferResponse brsp;
            if (!GatewayAuthClients::Instance().BeginPlayerTransfer(breq, &brsp) || !brsp.ok()) {
                return EncodeErr(req,
                                 brsp.message().empty() ? "begin transfer failed" : brsp.message(),
                                 response_frame);
            }
            transfer_id = brsp.transfer_id();
        } else {
            SessionStore::TransferBeginIn in;
            in.player_id = sticky.player_id;
            in.fence_token = sticky.fence_token;
            in.expected_route_version = sticky.route_version;
            in.from_logic = from_logic;
            in.to_logic = target_logic;
            in.map_instance_id = map_id;
            in.map_owner_epoch = epoch;
            in.gateway_instance_id = gw_id;
            SessionStore::TransferBeginOut out;
            if (!SessionStore::Instance().BeginPlayerTransfer(in, &out) || !out.ok)
                return EncodeErr(req, out.message.empty() ? "begin transfer failed" : out.message,
                                 response_frame);
            transfer_id = out.transfer_id;
        }

        glrpc::FreezePlayerRequest freq;
        freq.set_player_id(sticky.player_id);
        freq.set_session_id(sticky.session_id);
        freq.set_fence_token(sticky.fence_token);
        freq.set_transfer_id(transfer_id);
        freq.set_reason("enter_map_transfer");
        freq.set_idempotency_key(transfer_id + ":freeze");
        glrpc::FreezePlayerResponse frsp;
        if (!GatewayAuthClients::Instance().FreezePlayer(from_logic, freq, &frsp) || !frsp.ok()) {
            AbortTransfer(sticky.player_id, sticky.fence_token, transfer_id, from_logic);
            return EncodeErr(req, frsp.message().empty() ? "freeze failed" : frsp.message(),
                             response_frame);
        }

        // EXPORT_SNAPSHOT（Source 已 Freeze）
        glrpc::ExportPlayerSnapshotRequest xreq;
        xreq.set_player_id(sticky.player_id);
        xreq.set_session_id(sticky.session_id);
        xreq.set_fence_token(sticky.fence_token);
        xreq.set_transfer_id(transfer_id);
        xreq.set_target_gamelogic_id(target_logic);
        xreq.set_target_map_instance_id(map_id);
        xreq.set_target_owner_epoch(epoch);
        xreq.set_idempotency_key(transfer_id + ":export");
        glrpc::ExportPlayerSnapshotResponse xrsp;
        if (!GatewayAuthClients::Instance().ExportPlayerSnapshot(from_logic, xreq, &xrsp) ||
            !xrsp.ok() || !xrsp.has_snapshot()) {
            AbortTransfer(sticky.player_id, sticky.fence_token, transfer_id, from_logic);
            return EncodeErr(req, xrsp.message().empty() ? "export snapshot failed" : xrsp.message(),
                             response_frame);
        }

        glrpc::BindPlayerRequest prep;
        prep.set_request_id(sticky.connection_id);
        prep.set_player_id(sticky.player_id);
        prep.set_session_id(sticky.session_id);
        prep.set_fence_token(sticky.fence_token);
        prep.set_gamelogic_instance_id(target_logic);
        prep.set_map_instance_id(map_id);
        prep.set_map_owner_epoch(epoch);
        prep.set_route_version(sticky.route_version);
        prep.set_gateway_instance_id(gw_id);
        prep.set_generation(sticky.generation);
        prep.set_transfer_id(transfer_id);
        prep.set_idempotency_key(transfer_id + ":prepare");
        glrpc::BindPlayerResponse prsp;
        if (!GatewayAuthClients::Instance().BindPlayer(target_logic, prep, &prsp) || !prsp.ok()) {
            AbortTransfer(sticky.player_id, sticky.fence_token, transfer_id, from_logic);
            // 尝试解冻旧 Logic：再次 Bind 同 gen
            glrpc::BindPlayerRequest thaw = prep;
            thaw.set_gamelogic_instance_id(from_logic);
            thaw.set_transfer_id("");
            thaw.set_map_instance_id(sticky.map_instance_id);
            thaw.set_map_owner_epoch(sticky.owner_epoch);
            glrpc::BindPlayerResponse tr;
            GatewayAuthClients::Instance().BindPlayer(from_logic, thaw, &tr);
            return EncodeErr(req, prsp.message().empty() ? "prepare/bind failed" : prsp.message(),
                             response_frame);
        }

        // IMPORT_TARGET → TARGET_READY（路由切换前必须成功）
        glrpc::ImportPlayerSnapshotRequest ireq;
        *ireq.mutable_snapshot() = xrsp.snapshot();
        ireq.set_idempotency_key(transfer_id + ":import");
        glrpc::ImportPlayerSnapshotResponse irsp;
        if (!GatewayAuthClients::Instance().ImportPlayerSnapshot(target_logic, ireq, &irsp) ||
            !irsp.ok()) {
            AbortTransfer(sticky.player_id, sticky.fence_token, transfer_id, from_logic);
            glrpc::UnbindPlayerRequest ureq;
            ureq.set_player_id(sticky.player_id);
            ureq.set_session_id(sticky.session_id);
            ureq.set_fence_token(sticky.fence_token);
            ureq.set_reason("import_rollback");
            glrpc::UnbindPlayerResponse ursp;
            GatewayAuthClients::Instance().UnbindPlayer(target_logic, ureq, &ursp);
            glrpc::BindPlayerRequest thaw = prep;
            thaw.set_gamelogic_instance_id(from_logic);
            thaw.set_transfer_id("");
            thaw.set_map_instance_id(sticky.map_instance_id);
            thaw.set_map_owner_epoch(sticky.owner_epoch);
            glrpc::BindPlayerResponse tr;
            GatewayAuthClients::Instance().BindPlayer(from_logic, thaw, &tr);
            return EncodeErr(req, irsp.message().empty() ? "import snapshot failed" : irsp.message(),
                             response_frame);
        }
        LOG_INFO << "EnterMap snapshot imported player=" << sticky.player_id
                 << " transfer=" << transfer_id << " idempotent=" << irsp.already_applied();

        uint64_t committed_rv = 0;
        if (sess_rpc) {
            sess::CommitPlayerTransferRequest creq;
            creq.set_player_id(sticky.player_id);
            creq.set_fence_token(sticky.fence_token);
            creq.set_transfer_id(transfer_id);
            creq.set_to_gamelogic_instance_id(target_logic);
            creq.set_map_instance_id(map_id);
            creq.set_map_owner_epoch(epoch);
            creq.set_gateway_instance_id(gw_id);
            sess::CommitPlayerTransferResponse crsp;
            if (!GatewayAuthClients::Instance().CommitPlayerTransfer(creq, &crsp) || !crsp.ok()) {
                AbortTransfer(sticky.player_id, sticky.fence_token, transfer_id, from_logic);
                glrpc::UnbindPlayerRequest ureq;
                ureq.set_player_id(sticky.player_id);
                ureq.set_session_id(sticky.session_id);
                ureq.set_fence_token(sticky.fence_token);
                ureq.set_reason("prepare_rollback");
                glrpc::UnbindPlayerResponse ursp;
                GatewayAuthClients::Instance().UnbindPlayer(target_logic, ureq, &ursp);
                glrpc::BindPlayerRequest thaw;
                thaw.set_player_id(sticky.player_id);
                thaw.set_session_id(sticky.session_id);
                thaw.set_fence_token(sticky.fence_token);
                thaw.set_gamelogic_instance_id(from_logic);
                thaw.set_map_instance_id(sticky.map_instance_id);
                thaw.set_map_owner_epoch(sticky.owner_epoch);
                thaw.set_route_version(sticky.route_version);
                thaw.set_gateway_instance_id(gw_id);
                thaw.set_generation(sticky.generation);
                glrpc::BindPlayerResponse tr;
                GatewayAuthClients::Instance().BindPlayer(from_logic, thaw, &tr);
                return EncodeErr(
                    req, crsp.message().empty() ? "commit transfer failed" : crsp.message(),
                    response_frame);
            }
            committed_rv = crsp.route_version();
            if (route_out) {
                route_out->gamelogic_instance_id = crsp.gamelogic_instance_id();
                route_out->map_instance_id = crsp.map_instance_id();
                route_out->owner_epoch = crsp.map_owner_epoch();
                route_out->route_version = committed_rv;
            }
        } else {
            SessionStore::TransferCommitIn cin;
            cin.player_id = sticky.player_id;
            cin.fence_token = sticky.fence_token;
            cin.transfer_id = transfer_id;
            cin.to_logic = target_logic;
            cin.map_instance_id = map_id;
            cin.map_owner_epoch = epoch;
            cin.gateway_instance_id = gw_id;
            SessionStore::TransferCommitOut cout;
            if (!SessionStore::Instance().CommitPlayerTransfer(cin, &cout) || !cout.ok) {
                AbortTransfer(sticky.player_id, sticky.fence_token, transfer_id, from_logic);
                return EncodeErr(req, cout.message.empty() ? "commit failed" : cout.message,
                                 response_frame);
            }
            committed_rv = cout.route_version;
            if (route_out) {
                route_out->gamelogic_instance_id = cout.gamelogic_instance_id;
                route_out->map_instance_id = cout.map_instance_id;
                route_out->owner_epoch = cout.map_owner_epoch;
                route_out->route_version = committed_rv;
            }
        }
        LOG_INFO << "EnterMap transfer committed player=" << sticky.player_id
                 << " from=" << from_logic << " to=" << target_logic << " transfer=" << transfer_id
                 << " rv=" << committed_rv;
    } else {
        // 同 Owner：Gateway 写权威 Session 路由，避免 Logic 在 brpc yield 后二次 Update 踩 fence
        uint64_t new_rv = 0;
        if (GatewayAuthClients::Instance().ready()) {
            sess::UpdatePlayerRouteRequest ureq;
            ureq.set_player_id(sticky.player_id);
            ureq.set_fence_token(sticky.fence_token);
            ureq.set_gamelogic_instance_id(target_logic);
            ureq.set_map_instance_id(map_id);
            ureq.set_map_owner_epoch(epoch);
            ureq.set_route_version(0);  // Lua：auto cur+1
            ureq.set_gateway_instance_id(gw_id);
            sess::UpdatePlayerRouteResponse ursp;
            if (!GatewayAuthClients::Instance().UpdatePlayerRoute(ureq, &ursp)) {
                return EncodeErr(req,
                                 ursp.message().empty() ? "update route failed" : ursp.message(),
                                 response_frame);
            }
            new_rv = ursp.route_version();
        } else if (SessionStore::Instance().Available()) {
            std::string uerr;
            if (!SessionStore::Instance().UpdatePlayerRoute(
                    sticky.player_id, sticky.fence_token, target_logic, map_id, epoch, 0, gw_id, "",
                    &new_rv, &uerr)) {
                return EncodeErr(req, uerr.empty() ? "update route failed" : uerr, response_frame);
            }
        } else {
            return EncodeErr(req, "session not ready for route update", response_frame);
        }
        if (route_out) {
            route_out->gamelogic_instance_id = target_logic;
            route_out->map_instance_id = map_id;
            route_out->owner_epoch = epoch;
            route_out->route_version = new_rv;
        }
    }

    // Dispatch EnterMap 到（新）Owner；禁止在未 Bind 时直达
    SessionHandle dispatch_h = sticky;
    if (route_out) {
        dispatch_h = *route_out;
        dispatch_h.player_id = sticky.player_id;
        dispatch_h.session_id = sticky.session_id;
        dispatch_h.fence_token = sticky.fence_token;
        dispatch_h.generation = sticky.generation;
        dispatch_h.connection_id = sticky.connection_id;
    }
    // 权威目标以 Resolve/Transfer 为准，避免 Commit 回填空字段时打到错误 Logic
    dispatch_h.gamelogic_instance_id = target_logic;
    dispatch_h.map_instance_id = map_id;
    dispatch_h.owner_epoch = epoch;
    if (placement_rv != 0)
        dispatch_h.route_version = placement_rv;

    glrpc::ClientCommand cmd;
    cmd.set_request_id(sticky.connection_id);
    cmd.set_player_id(sticky.player_id);
    cmd.set_session_id(sticky.session_id);
    cmd.set_fence_token(sticky.fence_token);
    cmd.set_gamelogic_instance_id(dispatch_h.gamelogic_instance_id);
    cmd.set_map_instance_id(map_id);
    cmd.set_map_owner_epoch(epoch);
    cmd.set_route_version(dispatch_h.route_version);
    cmd.set_generation(sticky.generation);
    cmd.set_payload(request_payload);
    cmd.set_message_type("enter_map");
    cmd.set_client_seq(req.seq());
    cmd.set_deadline_ms(3000);
    glrpc::CommandResult result;
    if (!GatewayAuthClients::Instance().Dispatch(dispatch_h.gamelogic_instance_id, cmd, &result) ||
        !result.ok()) {
        if (cross) {
            // Commit 已成功：不回滚 Session（避免双写窗口）；EnterMap 可幂等重试
            LOG_WARN << "EnterMap dispatch after commit failed player=" << sticky.player_id
                     << " msg=" << result.message();
        }
        if (!result.response_frame().empty()) {
            *response_frame = result.response_frame();
            return false;
        }
        return EncodeErr(req, result.message().empty() ? "enter_map dispatch failed" : result.message(),
                         response_frame);
    }
    *response_frame = result.response_frame();

    // 从 EnterMapRsp 刷新路由（同 Owner 路径也会 UpdatePlayerRoute）
    {
        std::string buf = *response_frame;
        std::string payload;
        if (DecodeOneFrame(&buf, &payload) == FrameDecodeResult::Complete) {
            game::GameResponse gr;
            if (gr.ParseFromString(payload) && gr.ok() && gr.has_enter_map() && gr.enter_map().ok() &&
                route_out) {
                if (!gr.enter_map().gamelogic_instance_id().empty())
                    route_out->gamelogic_instance_id = gr.enter_map().gamelogic_instance_id();
                if (gr.enter_map().map_instance_id() != 0)
                    route_out->map_instance_id = gr.enter_map().map_instance_id();
                if (gr.enter_map().owner_epoch() != 0)
                    route_out->owner_epoch = gr.enter_map().owner_epoch();
                if (gr.enter_map().route_version() != 0)
                    route_out->route_version = gr.enter_map().route_version();
            }
        }
    }

    if (cross) {
        glrpc::UnbindPlayerRequest ureq;
        ureq.set_player_id(sticky.player_id);
        ureq.set_session_id(sticky.session_id);
        ureq.set_fence_token(sticky.fence_token);
        ureq.set_reason("transfer_finalize");
        ureq.set_idempotency_key(transfer_id + ":finalize");
        glrpc::UnbindPlayerResponse ursp;
        if (!GatewayAuthClients::Instance().UnbindPlayer(from_logic, ureq, &ursp)) {
            LOG_WARN << "FinalizeUnbind timeout/fail player=" << sticky.player_id
                     << " old=" << from_logic << " (Session already on new owner; no dual-write)";
        }
    }
    return true;
}

bool BeginOrchestrateGatewayEnterMap(const SessionHandle &sticky, const std::string &request_payload,
                                     GatewayEnterMapDone done) {
    if (!done)
        return false;
    const uint64_t shard_key = sticky.player_id;
    PlayerSerialQueue::Instance().MarkAsyncInFlight(shard_key);
    struct Holder {
        SessionHandle sticky;
        std::string payload;
        uint64_t shard_key = 0;
        GatewayEnterMapDone done;
    };
    auto h = std::make_shared<Holder>();
    h->sticky = sticky;
    h->payload = request_payload;
    h->shard_key = shard_key;
    h->done = std::move(done);
    if (!RpcOffloadPool::Instance().TryPost([h]() {
            std::string out;
            SessionHandle route = h->sticky;
            const bool ok = OrchestrateGatewayEnterMap(h->sticky, h->payload, &out, &route);
            if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(
                    h->shard_key, [h, ok, out = std::move(out),
                                   route = std::move(route)]() mutable {
                        h->done(ok, std::move(out), std::move(route));
                    })) {
                (void)ok;
            }
        })) {
        PlayerSerialQueue::Instance().ClearAsyncInFlight(shard_key);
        return false;
    }
    return true;
}

bool OrchestrateGatewayQueryMapLines(const SessionHandle &sticky, const std::string &request_payload,
                                     std::string *response_frame) {
    game::GameRequest req;
    if (!req.ParseFromString(request_payload) || !req.has_query_map_lines()) {
        game::GameResponse rsp;
        rsp.set_ok(false);
        rsp.set_message("invalid query_map_lines");
        rsp.set_error_code(gameproto::kErrInvalidArgument);
        std::string raw;
        return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
    }
    const auto &q = req.query_map_lines();
    game::GameResponse rsp;
    rsp.set_seq(req.seq());
    auto *body = rsp.mutable_query_map_lines();
    body->set_map_template_id(q.map_template_id());
    sess::QueryMapLinesRequest sreq;
    sreq.set_realm_id(MapLineView::EffectiveRealm(q.realm_id()));
    sreq.set_map_template_id(q.map_template_id());
    sess::QueryMapLinesResponse srsp;
    bool ok = false;
    if (GatewayAuthClients::Instance().ready()) {
        ok = GatewayAuthClients::Instance().QueryMapLines(sreq, &srsp) && srsp.ok();
    } else if (PlacementStore::Instance().Available()) {
        std::vector<MapLineInfo> rows;
        ok = PlacementStore::Instance().ListLines(MapLineView::EffectiveRealm(q.realm_id()),
                                                 q.map_template_id(), &rows);
        srsp.set_ok(ok);
        srsp.set_message(ok ? "ok" : "list failed");
        MapScenePolicy pol;
        if (MapCatalog::Instance().GetScenePolicy(q.map_template_id(), &pol))
            srsp.set_kind(SceneKindToString(pol.kind));
        else
            srsp.set_kind("LEGACY_POOL");
        for (const auto &r : rows)
            MapLineView::CopyLine(r, srsp.add_lines());
    }
    if (!ok) {
        rsp.set_ok(false);
        body->set_ok(false);
        body->set_message(srsp.message().empty() ? "query map lines failed" : srsp.message());
        body->set_error_code(srsp.error_code().empty() ? gameproto::kErrDependencyUnavailable
                                                       : srsp.error_code());
        rsp.set_message(body->message());
        gameproto::PromotePublicError(&rsp, 0);
        std::string raw;
        return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
    }
    body->set_ok(true);
    body->set_message("ok");
    body->set_kind(srsp.kind());
    for (int i = 0; i < srsp.lines_size(); ++i)
        *body->add_lines() = srsp.lines(i);
    rsp.set_ok(true);
    rsp.set_message("ok");
    gameproto::PromotePublicError(&rsp, 0);
    (void)sticky;
    std::string raw;
    return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
}

bool BeginOrchestrateGatewayQueryMapLines(const SessionHandle &sticky,
                                          const std::string &request_payload,
                                          GatewayEnterMapDone done) {
    if (!done)
        return false;
    const uint64_t shard_key = sticky.player_id;
    PlayerSerialQueue::Instance().MarkAsyncInFlight(shard_key);
    struct Holder {
        SessionHandle sticky;
        std::string payload;
        uint64_t shard_key = 0;
        GatewayEnterMapDone done;
    };
    auto h = std::make_shared<Holder>();
    h->sticky = sticky;
    h->payload = request_payload;
    h->shard_key = shard_key;
    h->done = std::move(done);
    if (!RpcOffloadPool::Instance().TryPost([h]() {
            std::string out;
            SessionHandle route = h->sticky;
            const bool ok = OrchestrateGatewayQueryMapLines(h->sticky, h->payload, &out);
            if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(
                    h->shard_key, [h, ok, out = std::move(out),
                                   route = std::move(route)]() mutable {
                        h->done(ok, std::move(out), std::move(route));
                    })) {
                (void)ok;
            }
        })) {
        PlayerSerialQueue::Instance().ClearAsyncInFlight(shard_key);
        return false;
    }
    return true;
}

bool OrchestrateGatewayCreateDungeon(const SessionHandle &sticky, const std::string &request_payload,
                                     std::string *response_frame) {
    game::GameRequest req;
    if (!req.ParseFromString(request_payload) || !req.has_create_dungeon()) {
        game::GameResponse rsp;
        rsp.set_ok(false);
        rsp.set_message("invalid create_dungeon");
        rsp.set_error_code(gameproto::kErrInvalidArgument);
        std::string raw;
        return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
    }
    const auto &q = req.create_dungeon();
    game::GameResponse rsp;
    rsp.set_seq(req.seq());
    auto *body = rsp.mutable_create_dungeon();
    body->set_map_template_id(q.map_template_id());
    sess::CreateDungeonRequest sreq;
    sreq.set_realm_id(q.realm_id());
    sreq.set_map_template_id(q.map_template_id());
    sreq.set_player_id(q.player_id() != 0 ? q.player_id() : sticky.player_id);
    sreq.set_operation_id(q.operation_id());
    for (int i = 0; i < q.member_player_ids_size(); ++i)
        sreq.add_member_player_ids(q.member_player_ids(i));
    if (q.preferred_keep_logic() && !sticky.gamelogic_instance_id.empty())
        sreq.set_preferred_owner(sticky.gamelogic_instance_id);
    MapScenePolicy pol;
    if (MapCatalog::Instance().GetScenePolicy(q.map_template_id(), &pol) &&
        pol.kind == SceneKind::Dungeon) {
        sreq.set_soft_cap(pol.soft_cap);
        sreq.set_hard_cap(pol.hard_cap);
        sreq.set_empty_close_delay(pol.empty_close_delay);
    }
    sess::CreateDungeonResponse srsp;
    bool ok = false;
    if (GatewayAuthClients::Instance().ready()) {
        ok = GatewayAuthClients::Instance().CreateDungeon(sreq, &srsp) && srsp.ok();
    } else if (PlacementStore::Instance().Available()) {
        CreateDungeonInput in;
        in.realm_id = sreq.realm_id();
        in.map_template_id = sreq.map_template_id();
        in.player_id = sreq.player_id();
        in.operation_id = sreq.operation_id();
        in.preferred_owner = sreq.preferred_owner();
        in.soft_cap = sreq.soft_cap();
        in.hard_cap = sreq.hard_cap();
        in.empty_close_delay = sreq.empty_close_delay();
        for (int i = 0; i < sreq.member_player_ids_size(); ++i)
            in.member_player_ids.push_back(sreq.member_player_ids(i));
        CreateDungeonResult result;
        ok = PlacementStore::Instance().CreateDungeon(in, &result) && result.ok;
        srsp.set_ok(ok);
        srsp.set_message(ok ? "ok" : result.message);
        srsp.set_error_code(result.error_code);
        if (ok) {
            auto *p = srsp.mutable_placement();
            p->set_map_template_id(result.placement.map_template_id);
            p->set_map_instance_id(result.placement.map_instance_id);
            p->set_owner_logic_server_id(result.placement.owner_logic_server_id);
            p->set_owner_epoch(result.placement.owner_epoch);
            p->set_route_version(result.placement.route_version);
            for (uint64_t pid : result.member_player_ids)
                srsp.add_member_player_ids(pid);
        }
    }
    if (!ok) {
        rsp.set_ok(false);
        body->set_ok(false);
        body->set_message(srsp.message().empty() ? "create dungeon failed" : srsp.message());
        body->set_error_code(srsp.error_code().empty() ? gameproto::kErrDependencyUnavailable
                                                       : srsp.error_code());
        rsp.set_message(body->message());
        gameproto::PromotePublicError(&rsp, 0);
        std::string raw;
        return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
    }
    body->set_ok(true);
    body->set_message("ok");
    if (srsp.has_placement()) {
        const auto &p = srsp.placement();
        body->set_map_template_id(p.map_template_id());
        body->set_map_instance_id(p.map_instance_id());
        body->set_gamelogic_instance_id(p.owner_logic_server_id());
        body->set_owner_epoch(p.owner_epoch());
        body->set_route_version(p.route_version());
    }
    for (int i = 0; i < srsp.member_player_ids_size(); ++i)
        body->add_member_player_ids(srsp.member_player_ids(i));
    rsp.set_ok(true);
    rsp.set_message("ok");
    gameproto::PromotePublicError(&rsp, 0);
    std::string raw;
    return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
}

bool BeginOrchestrateGatewayCreateDungeon(const SessionHandle &sticky,
                                          const std::string &request_payload,
                                          GatewayEnterMapDone done) {
    if (!done)
        return false;
    const uint64_t shard_key = sticky.player_id;
    PlayerSerialQueue::Instance().MarkAsyncInFlight(shard_key);
    struct Holder {
        SessionHandle sticky;
        std::string payload;
        uint64_t shard_key = 0;
        GatewayEnterMapDone done;
    };
    auto h = std::make_shared<Holder>();
    h->sticky = sticky;
    h->payload = request_payload;
    h->shard_key = shard_key;
    h->done = std::move(done);
    if (!RpcOffloadPool::Instance().TryPost([h]() {
            std::string out;
            SessionHandle route = h->sticky;
            const bool ok = OrchestrateGatewayCreateDungeon(h->sticky, h->payload, &out);
            if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(
                    h->shard_key, [h, ok, out = std::move(out),
                                   route = std::move(route)]() mutable {
                        h->done(ok, std::move(out), std::move(route));
                    })) {
                (void)ok;
            }
        })) {
        PlayerSerialQueue::Instance().ClearAsyncInFlight(shard_key);
        return false;
    }
    return true;
}

bool OrchestrateGatewaySwitchLine(const SessionHandle &sticky, const std::string &request_payload,
                                  std::string *response_frame, SessionHandle *route_out) {
    if (route_out)
        *route_out = sticky;
    game::GameRequest req;
    if (!req.ParseFromString(request_payload) || !req.has_switch_line()) {
        game::GameResponse rsp;
        rsp.set_ok(false);
        rsp.set_message("invalid switch_line");
        rsp.set_error_code(gameproto::kErrInvalidArgument);
        std::string raw;
        return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
    }
    const auto &sw = req.switch_line();
    auto fail = [&](const std::string &msg, const std::string &code) {
        game::GameResponse rsp;
        rsp.set_seq(req.seq());
        rsp.set_ok(false);
        rsp.set_message(msg);
        rsp.set_error_code(code);
        auto *body = rsp.mutable_switch_line();
        body->set_ok(false);
        body->set_message(msg);
        body->set_error_code(code);
        gameproto::PromotePublicError(&rsp, 0);
        std::string raw;
        return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
    };
    if (sticky.player_id == 0 || sticky.fence_token.empty())
        return fail("unauthenticated_or_no_route", gameproto::kErrUnauthenticated);
    if (sw.line_no() == 0 || sw.map_template_id() == 0)
        return fail("line_no and map_template_id required", gameproto::kErrInvalidArgument);

    sess::SwitchLineRequest sreq;
    sreq.set_realm_id(sw.realm_id());
    sreq.set_map_template_id(sw.map_template_id());
    sreq.set_player_id(sw.player_id() != 0 ? sw.player_id() : sticky.player_id);
    sreq.set_line_no(sw.line_no());
    sreq.set_operation_id(sw.operation_id());
    MapScenePolicy pol;
    if (MapCatalog::Instance().GetScenePolicy(sw.map_template_id(), &pol) &&
        pol.kind == SceneKind::Line) {
        sreq.set_soft_cap(pol.soft_cap);
        sreq.set_hard_cap(pol.hard_cap);
    }
    sess::SwitchLineResponse srsp;
    bool ok = false;
    if (GatewayAuthClients::Instance().ready()) {
        ok = GatewayAuthClients::Instance().SwitchLine(sreq, &srsp) && srsp.ok();
    } else if (PlacementStore::Instance().Available()) {
        SwitchLineInput in;
        in.realm_id = sreq.realm_id();
        in.map_template_id = sreq.map_template_id();
        in.player_id = sreq.player_id();
        in.line_no = sreq.line_no();
        in.operation_id = sreq.operation_id();
        in.soft_cap = sreq.soft_cap();
        in.hard_cap = sreq.hard_cap();
        ResolveOrCreateResult result;
        ok = PlacementStore::Instance().SwitchLine(in, &result) && result.ok;
        srsp.set_ok(ok);
        srsp.set_message(ok ? "ok" : result.message);
        srsp.set_error_code(result.error_code);
        if (ok) {
            auto *p = srsp.mutable_placement();
            p->set_map_template_id(result.placement.map_template_id);
            p->set_map_instance_id(result.placement.map_instance_id);
            p->set_owner_logic_server_id(result.placement.owner_logic_server_id);
            p->set_owner_epoch(result.placement.owner_epoch);
            p->set_route_version(result.placement.route_version);
            p->set_kind(result.placement.kind);
            p->set_line_no(result.placement.line_no);
            p->set_occupancy(result.occupancy);
            p->set_soft_cap(result.placement.soft_cap);
            p->set_hard_cap(result.placement.hard_cap);
            srsp.set_occupancy(result.occupancy);
        }
    }
    if (!ok)
        return fail(srsp.message().empty() ? "switch line failed" : srsp.message(),
                    srsp.error_code().empty() ? gameproto::kErrDependencyUnavailable
                                              : srsp.error_code());

    game::GameRequest enter;
    enter.set_seq(req.seq());
    enter.set_session_token(req.session_token());
    auto *e = enter.mutable_enter_map();
    e->set_player_id(sreq.player_id());
    e->set_realm_id(sreq.realm_id());
    e->set_map_template_id(sreq.map_template_id());
    e->set_map_instance_id(srsp.placement().map_instance_id());
    e->set_operation_id(sw.operation_id());
    e->set_line_no(sw.line_no());
    std::string enter_payload;
    if (!enter.SerializeToString(&enter_payload))
        return fail("serialize enter_map failed", gameproto::kErrInternal);
    std::string enter_frame;
    SessionHandle route = sticky;
    const bool entered = OrchestrateGatewayEnterMap(sticky, enter_payload, &enter_frame, &route);
    if (route_out)
        *route_out = route;
    std::string buf = enter_frame;
    std::string payload;
    game::GameResponse gr;
    if (DecodeOneFrame(&buf, &payload) == FrameDecodeResult::Complete && gr.ParseFromString(payload)) {
        game::GameResponse out;
        out.set_seq(req.seq());
        out.set_ok(gr.ok());
        out.set_message(gr.message());
        out.set_error_code(gr.error_code());
        auto *body = out.mutable_switch_line();
        if (gr.has_enter_map()) {
            const auto &em = gr.enter_map();
            body->set_ok(em.ok());
            body->set_message(em.message());
            body->set_error_code(em.error_code());
            body->set_map_template_id(em.map_template_id());
            body->set_map_instance_id(em.map_instance_id());
            body->set_gamelogic_instance_id(em.gamelogic_instance_id());
            body->set_owner_epoch(em.owner_epoch());
            body->set_route_version(em.route_version());
            body->set_kind(em.kind().empty() ? srsp.placement().kind() : em.kind());
            body->set_line_no(em.line_no() != 0 ? em.line_no() : srsp.placement().line_no());
            body->set_occupancy(em.occupancy() != 0 ? em.occupancy() : srsp.occupancy());
            body->set_soft_cap(em.soft_cap() != 0 ? em.soft_cap() : srsp.placement().soft_cap());
            body->set_hard_cap(em.hard_cap() != 0 ? em.hard_cap() : srsp.placement().hard_cap());
            *body->mutable_spawn_position() = em.spawn_position();
            body->set_spawn_yaw(em.spawn_yaw());
            *body->mutable_self() = em.self();
            for (int i = 0; i < em.aoi_snapshot_size(); ++i)
                *body->add_aoi_snapshot() = em.aoi_snapshot(i);
            for (int i = 0; i < em.lines_size(); ++i)
                *body->add_lines() = em.lines(i);
            if (body->lines_size() == 0 && body->kind() == "LINE")
                MapLineView::FillLines(sw.realm_id(), sw.map_template_id(), body->mutable_lines());
        } else {
            body->set_ok(false);
            body->set_message(gr.message());
            body->set_error_code(gr.error_code());
        }
        gameproto::PromotePublicError(&out, 0);
        std::string raw;
        return out.SerializeToString(&raw) && EncodeFrame(raw, response_frame) && entered &&
               gr.ok();
    }
    if (!enter_frame.empty()) {
        *response_frame = enter_frame;
        return entered;
    }
    return fail("switch_line enter failed", gameproto::kErrInternal);
}

bool BeginOrchestrateGatewaySwitchLine(const SessionHandle &sticky,
                                       const std::string &request_payload,
                                       GatewayEnterMapDone done) {
    if (!done)
        return false;
    const uint64_t shard_key = sticky.player_id;
    PlayerSerialQueue::Instance().MarkAsyncInFlight(shard_key);
    struct Holder {
        SessionHandle sticky;
        std::string payload;
        uint64_t shard_key = 0;
        GatewayEnterMapDone done;
    };
    auto h = std::make_shared<Holder>();
    h->sticky = sticky;
    h->payload = request_payload;
    h->shard_key = shard_key;
    h->done = std::move(done);
    if (!RpcOffloadPool::Instance().TryPost([h]() {
            std::string out;
            SessionHandle route = h->sticky;
            const bool ok = OrchestrateGatewaySwitchLine(h->sticky, h->payload, &out, &route);
            if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(
                    h->shard_key, [h, ok, out = std::move(out),
                                   route = std::move(route)]() mutable {
                        h->done(ok, std::move(out), std::move(route));
                    })) {
                (void)ok;
            }
        })) {
        PlayerSerialQueue::Instance().ClearAsyncInFlight(shard_key);
        return false;
    }
    return true;
}

bool OrchestrateGatewayEnqueueMap(const SessionHandle &sticky, const std::string &request_payload,
                                  std::string *response_frame) {
    game::GameRequest req;
    if (!req.ParseFromString(request_payload) || !req.has_enqueue_map()) {
        game::GameResponse rsp;
        rsp.set_ok(false);
        rsp.set_message("invalid enqueue_map");
        rsp.set_error_code(gameproto::kErrInvalidArgument);
        std::string raw;
        return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
    }
    const auto &q = req.enqueue_map();
    game::GameResponse rsp;
    rsp.set_seq(req.seq());
    auto *body = rsp.mutable_enqueue_map();
    sess::EnqueueMapRequest sreq;
    sreq.set_realm_id(q.realm_id());
    sreq.set_map_template_id(q.map_template_id());
    sreq.set_player_id(q.player_id() != 0 ? q.player_id() : sticky.player_id);
    sreq.set_line_no(q.line_no());
    sreq.set_queue_token(q.queue_token());
    MapScenePolicy pol;
    if (MapCatalog::Instance().GetScenePolicy(q.map_template_id(), &pol))
        sreq.set_hard_cap(pol.hard_cap);
    sess::EnqueueMapResponse srsp;
    bool ok = false;
    if (GatewayAuthClients::Instance().ready()) {
        ok = GatewayAuthClients::Instance().EnqueueMap(sreq, &srsp) && srsp.ok();
    } else if (PlacementStore::Instance().Available()) {
        EnqueueMapInput in;
        in.realm_id = sreq.realm_id();
        in.map_template_id = sreq.map_template_id();
        in.player_id = sreq.player_id();
        in.line_no = sreq.line_no();
        in.queue_token = sreq.queue_token();
        in.hard_cap = sreq.hard_cap();
        EnqueueMapResult result;
        ok = PlacementStore::Instance().EnqueueMap(in, &result) && result.ok;
        srsp.set_ok(ok);
        srsp.set_message(ok ? "ok" : result.message);
        srsp.set_error_code(result.error_code);
        srsp.set_queue_token(result.queue_token);
        srsp.set_queue_position(result.queue_position);
        srsp.set_queue_length(result.queue_length);
        srsp.set_line_no(result.line_no);
        srsp.set_ready(result.ready);
    }
    if (!ok) {
        rsp.set_ok(false);
        body->set_ok(false);
        body->set_message(srsp.message().empty() ? "enqueue map failed" : srsp.message());
        body->set_error_code(srsp.error_code().empty() ? gameproto::kErrDependencyUnavailable
                                                       : srsp.error_code());
        rsp.set_message(body->message());
        gameproto::PromotePublicError(&rsp, 0);
        std::string raw;
        return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
    }
    body->set_ok(true);
    body->set_message("ok");
    body->set_queue_token(srsp.queue_token());
    body->set_queue_position(srsp.queue_position());
    body->set_queue_length(srsp.queue_length());
    body->set_line_no(srsp.line_no());
    body->set_ready(srsp.ready());
    rsp.set_ok(true);
    rsp.set_message("ok");
    gameproto::PromotePublicError(&rsp, 0);
    (void)sticky;
    std::string raw;
    return rsp.SerializeToString(&raw) && EncodeFrame(raw, response_frame);
}

bool BeginOrchestrateGatewayEnqueueMap(const SessionHandle &sticky,
                                       const std::string &request_payload,
                                       GatewayEnterMapDone done) {
    if (!done)
        return false;
    const uint64_t shard_key = sticky.player_id;
    PlayerSerialQueue::Instance().MarkAsyncInFlight(shard_key);
    struct Holder {
        SessionHandle sticky;
        std::string payload;
        uint64_t shard_key = 0;
        GatewayEnterMapDone done;
    };
    auto h = std::make_shared<Holder>();
    h->sticky = sticky;
    h->payload = request_payload;
    h->shard_key = shard_key;
    h->done = std::move(done);
    if (!RpcOffloadPool::Instance().TryPost([h]() {
            std::string out;
            SessionHandle route = h->sticky;
            const bool ok = OrchestrateGatewayEnqueueMap(h->sticky, h->payload, &out);
            if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(
                    h->shard_key, [h, ok, out = std::move(out),
                                   route = std::move(route)]() mutable {
                        h->done(ok, std::move(out), std::move(route));
                    })) {
                (void)ok;
            }
        })) {
        PlayerSerialQueue::Instance().ClearAsyncInFlight(shard_key);
        return false;
    }
    return true;
}

}  // namespace gameproto
