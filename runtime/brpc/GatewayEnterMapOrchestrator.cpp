#include "GatewayEnterMapOrchestrator.h"

#include "GatewayAuthClients.h"
#include "GatewayIdentity.h"
#include "Logging.h"
#include "PlacementStore.h"
#include "ProtoFraming.h"
#include "SessionStore.h"
#include "game.pb.h"
#include "session.pb.h"

namespace gameproto {

namespace {

bool EncodeErr(const game::GameRequest &req, const std::string &msg, std::string *frame) {
    game::GameResponse rsp;
    rsp.set_seq(req.seq());
    rsp.set_ok(false);
    rsp.set_message(msg);
    auto *body = rsp.mutable_enter_map();
    body->set_ok(false);
    body->set_message(msg);
    std::string raw;
    return rsp.SerializeToString(&raw) && EncodeFrame(raw, frame);
}

bool ResolveTarget(const game::EnterMapReq &e, std::string *owner, uint64_t *map_id,
                   uint64_t *epoch, uint64_t *placement_rv, std::string *err) {
    if (GatewayAuthClients::Instance().ready()) {
        sess::ResolveOrCreateMapRequest req;
        req.set_realm_id(e.realm_id());
        req.set_map_template_id(e.map_template_id());
        req.set_map_instance_id(e.map_instance_id());
        sess::ResolveOrCreateMapResponse rsp;
        if (!GatewayAuthClients::Instance().ResolveOrCreateMap(req, &rsp) || !rsp.ok()) {
            if (err)
                *err = rsp.message().empty() ? "resolve map failed" : rsp.message();
            return false;
        }
        const auto &p = rsp.placement();
        if (p.state() == "RECOVERING" || p.state() == "CLOSED" || p.state() == "FROZEN") {
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
        ResolveOrCreateResult result;
        if (!PlacementStore::Instance().ResolveOrCreate(in, &result) || !result.ok) {
            if (err)
                *err = result.message.empty() ? "placement resolve failed" : result.message;
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
    if (!ResolveTarget(req.enter_map(), &target_logic, &map_id, &epoch, &placement_rv, &err))
        return EncodeErr(req, err, response_frame);

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
        if (!GatewayAuthClients::Instance().FreezePlayer(from_logic, freq, &frsp)) {
            AbortTransfer(sticky.player_id, sticky.fence_token, transfer_id, from_logic);
            return EncodeErr(req, frsp.message().empty() ? "freeze failed" : frsp.message(),
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
        if (!GatewayAuthClients::Instance().BindPlayer(target_logic, prep, &prsp)) {
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

}  // namespace gameproto
