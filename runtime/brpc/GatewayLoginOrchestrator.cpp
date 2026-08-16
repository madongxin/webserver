#include "GatewayLoginOrchestrator.h"

#include "GatewayAuthClients.h"
#include "GatewayConnRegistry.h"
#include "Logging.h"
#include "PlayerSerialQueue.h"
#include "ProtoFraming.h"
#include "PushReplayCache.h"
#include "RpcOffloadPool.h"
#include "SessionRpcClient.h"
#include "game.pb.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "PushReplayStore.h"
#endif
#include "session.pb.h"

#include <atomic>
#include <memory>
#include <utility>

namespace gameproto {

static bool EncodeResponse(const game::GameResponse &rsp, std::string *frame) {
    std::string body;
    if (!rsp.SerializeToString(&body))
        return false;
    return EncodeFrame(body, frame);
}

static void CompensateLogout(uint64_t player_id, const std::string &session_id,
                             const std::string &fence) {
    sess::LogoutRequest req;
    req.set_player_id(player_id);
    req.set_session_id(session_id);
    req.set_fence_token(fence);
    sess::LogoutResponse rsp;
    if (GatewayAuthClients::Instance().LogoutV2(req, &rsp)) {
        LOG_WARN << "CompensateLogoutV2 player=" << player_id << " ok=" << rsp.ok();
        return;
    }
    if (SessionRpcClient::Instance().ready()) {
        game::LogoutReq legacy;
        legacy.set_player_id(player_id);
        legacy.set_token(fence);
        game::LogoutRsp lr;
        SessionRpcClient::Instance().Logout(legacy, &lr);
        LOG_WARN << "CompensateLogout legacy player=" << player_id << " ok=" << lr.ok();
    }
}

void CompensateGatewaySession(uint64_t player_id, const std::string &session_id,
                              const std::string &fence_token, uint64_t generation) {
    sess::MarkDisconnectedRequest req;
    req.set_player_id(player_id);
    req.set_session_id(session_id);
    req.set_fence_token(fence_token);
    req.set_generation(generation);
    sess::MarkDisconnectedResponse rsp;
    if (GatewayAuthClients::Instance().MarkDisconnectedV2(req, &rsp)) {
        LOG_WARN << "Compensate MarkDisconnected player=" << player_id << " ok=" << rsp.ok();
        return;
    }
    if (SessionRpcClient::Instance().ready()) {
        SessionRpcClient::Instance().MarkDisconnected(player_id, fence_token, generation);
        LOG_WARN << "Compensate MarkDisconnected rpc player=" << player_id;
    }
}

bool OrchestrateGatewayLogin(const std::string &gateway_instance_id, uint64_t connection_id,
                             const std::string &request_payload, std::string *response_frame,
                             GatewayLoginRoute *route_out) {
    if (route_out)
        *route_out = GatewayLoginRoute{};
    game::GameRequest req;
    game::GameResponse rsp;
    if (!req.ParseFromString(request_payload) || !req.has_login()) {
        rsp.set_ok(false);
        rsp.set_message("invalid login payload");
        return EncodeResponse(rsp, response_frame);
    }
    rsp.set_seq(req.seq());
    auto *login_body = rsp.mutable_login();

    if (!GatewayAuthClients::Instance().ready()) {
        login_body->set_ok(false);
        login_body->set_message("auth/session client not ready");
        rsp.set_ok(false);
        rsp.set_message(login_body->message());
        return EncodeResponse(rsp, response_frame);
    }

    auth::LoginRequest areq;
    areq.set_player_id(req.login().player_id());
    areq.set_device_id(req.login().device_id());
    areq.set_server_id(req.login().server_id());
    areq.set_credential(req.login().credential());
    auth::LoginResponse arsp;
    if (!GatewayAuthClients::Instance().AuthLogin(areq, &arsp) || !arsp.ok()) {
        login_body->set_ok(false);
        login_body->set_message(arsp.message().empty() ? "auth failed" : arsp.message());
        rsp.set_ok(false);
        rsp.set_message(login_body->message());
        return EncodeResponse(rsp, response_frame);
    }

    sess::AcquireSessionRequest sreq;
    sreq.set_account_id(arsp.account_id());
    sreq.set_player_id(arsp.player_id());
    sreq.set_device_id(req.login().device_id());
    sreq.set_server_id(req.login().server_id());
    sreq.set_ttl_sec(req.login().ttl_sec());
    sreq.set_kick_other_device(req.login().kick_other_device());
    sreq.set_gateway_instance_id(gateway_instance_id);
    // 同连接同登录请求的稳定幂等键：RPC 超时重试不会创建双 Session
    sreq.set_operation_id("acq:" + gateway_instance_id + ":" + std::to_string(connection_id) + ":" +
                          std::to_string(arsp.player_id()) + ":" + req.login().device_id());
    sess::AcquireSessionResponse srsp;
    if (!GatewayAuthClients::Instance().AcquireSession(sreq, &srsp) || !srsp.ok()) {
        login_body->set_ok(false);
        login_body->set_message(srsp.message().empty() ? "acquire session failed" : srsp.message());
        rsp.set_ok(false);
        rsp.set_message(login_body->message());
        return EncodeResponse(rsp, response_frame);
    }

    if (srsp.gamelogic_instance_id().empty()) {
        if (!srsp.previous_session_id().empty()) {
            sess::RestorePreviousSessionRequest rreq;
            rreq.set_player_id(arsp.player_id());
            rreq.set_current_fence_token(srsp.fence_token());
            rreq.set_previous_fence_token(srsp.previous_fence_token());
            rreq.set_previous_session_id(srsp.previous_session_id());
            rreq.set_previous_generation(srsp.previous_generation());
            rreq.set_previous_gateway_instance_id(srsp.previous_gateway_instance_id());
            rreq.set_previous_device_id(srsp.previous_device_id());
            rreq.set_previous_gamelogic_instance_id(srsp.previous_gamelogic_instance_id());
            rreq.set_previous_map_instance_id(srsp.previous_map_instance_id());
            rreq.set_previous_map_owner_epoch(srsp.previous_map_owner_epoch());
            rreq.set_previous_route_version(srsp.previous_route_version());
            rreq.set_previous_server_id(srsp.previous_server_id());
            rreq.set_previous_login_time_sec(srsp.previous_login_time_sec());
            rreq.set_operation_id(sreq.operation_id());
            sess::RestorePreviousSessionResponse rrsp;
            GatewayAuthClients::Instance().RestorePreviousSession(rreq, &rrsp);
        } else {
            CompensateLogout(arsp.player_id(), srsp.session_id(), srsp.fence_token());
        }
        login_body->set_ok(false);
        login_body->set_message("no logic assigned");
        rsp.set_ok(false);
        rsp.set_message(login_body->message());
        return EncodeResponse(rsp, response_frame);
    }

    glrpc::BindPlayerRequest breq;
    breq.set_request_id(connection_id);
    breq.set_player_id(arsp.player_id());
    breq.set_session_id(srsp.session_id());
    breq.set_fence_token(srsp.fence_token());
    breq.set_gamelogic_instance_id(srsp.gamelogic_instance_id());
    breq.set_map_instance_id(srsp.map_instance_id());
    breq.set_map_owner_epoch(srsp.map_owner_epoch());
    breq.set_route_version(srsp.route_version());
    breq.set_gateway_instance_id(gateway_instance_id);
    breq.set_generation(srsp.generation());
    breq.set_idempotency_key(srsp.session_id() + ":bind");
    glrpc::BindPlayerResponse brsp;
    if (!GatewayAuthClients::Instance().BindPlayer(srsp.gamelogic_instance_id(), breq, &brsp) ||
        !brsp.ok()) {
        if (!srsp.previous_session_id().empty()) {
            sess::RestorePreviousSessionRequest rreq;
            rreq.set_player_id(arsp.player_id());
            rreq.set_current_fence_token(srsp.fence_token());
            rreq.set_previous_fence_token(srsp.previous_fence_token());
            rreq.set_previous_session_id(srsp.previous_session_id());
            rreq.set_previous_generation(srsp.previous_generation());
            rreq.set_previous_gateway_instance_id(srsp.previous_gateway_instance_id());
            rreq.set_previous_device_id(srsp.previous_device_id());
            rreq.set_previous_gamelogic_instance_id(srsp.previous_gamelogic_instance_id());
            rreq.set_previous_map_instance_id(srsp.previous_map_instance_id());
            rreq.set_previous_map_owner_epoch(srsp.previous_map_owner_epoch());
            rreq.set_previous_route_version(srsp.previous_route_version());
            rreq.set_previous_server_id(srsp.previous_server_id());
            rreq.set_previous_login_time_sec(srsp.previous_login_time_sec());
            rreq.set_operation_id(sreq.operation_id());
            sess::RestorePreviousSessionResponse rrsp;
            GatewayAuthClients::Instance().RestorePreviousSession(rreq, &rrsp);
            LOG_WARN << "Bind failed, restore previous player=" << arsp.player_id()
                     << " ok=" << rrsp.ok();
        } else {
            CompensateLogout(arsp.player_id(), srsp.session_id(), srsp.fence_token());
        }
        login_body->set_ok(false);
        login_body->set_message(brsp.message().empty() ? "bind player failed" : brsp.message());
        rsp.set_ok(false);
        rsp.set_message(login_body->message());
        return EncodeResponse(rsp, response_frame);
    }

    if (!srsp.previous_gateway_instance_id().empty() &&
        (!srsp.previous_session_id().empty() || srsp.previous_generation() != 0)) {
        sess::NotifySessionReplacedRequest nreq;
        nreq.set_player_id(arsp.player_id());
        nreq.set_previous_gateway_instance_id(srsp.previous_gateway_instance_id());
        nreq.set_previous_session_id(srsp.previous_session_id());
        nreq.set_previous_generation(srsp.previous_generation());
        nreq.set_reason("SESSION_REPLACED");
        sess::NotifySessionReplacedResponse nrsp;
        GatewayAuthClients::Instance().NotifySessionReplaced(nreq, &nrsp);
    }

    if (SessionRpcClient::Instance().ready()) {
        SessionRpcClient::Instance().BindConnection(arsp.player_id(), srsp.fence_token(),
                                                    gateway_instance_id, connection_id);
    }

    if (route_out) {
        route_out->player_id = arsp.player_id();
        route_out->session_id = srsp.session_id();
        route_out->fence_token = srsp.fence_token();
        route_out->generation = srsp.generation();
        route_out->gamelogic_instance_id = srsp.gamelogic_instance_id();
        route_out->map_instance_id = srsp.map_instance_id();
        route_out->map_owner_epoch = srsp.map_owner_epoch();
        route_out->route_version = srsp.route_version();
    }

    login_body->set_ok(true);
    login_body->set_message("login ok");
    login_body->set_token(srsp.fence_token());
    login_body->set_server_id(srsp.server_id());
    login_body->set_login_time_sec(srsp.login_time_sec());
    login_body->set_kicked_previous(srsp.kicked_previous());
    login_body->set_session_id(srsp.session_id());
    login_body->set_generation(srsp.generation());
    if (!brsp.profile_pb().empty())
        login_body->mutable_profile()->ParseFromString(brsp.profile_pb());
    rsp.set_ok(true);
    rsp.set_message("login ok");
    LOG_INFO << "Gateway login orchestrated player=" << arsp.player_id()
             << " logic=" << srsp.gamelogic_instance_id() << " conn=" << connection_id;
    return EncodeResponse(rsp, response_frame);
}

bool OrchestrateGatewayRegister(const std::string &request_payload, std::string *response_frame) {
    game::GameRequest req;
    game::GameResponse rsp;
    if (!req.ParseFromString(request_payload) || !req.has_register_()) {
        rsp.set_ok(false);
        rsp.set_message("invalid register payload");
        return EncodeResponse(rsp, response_frame);
    }
    rsp.set_seq(req.seq());
    auto *body = rsp.mutable_register_();
    if (!GatewayAuthClients::Instance().ready()) {
        body->set_ok(false);
        body->set_message("auth client not ready");
        rsp.set_ok(false);
        rsp.set_message(body->message());
        return EncodeResponse(rsp, response_frame);
    }
    auth::RegisterRequest areq;
    areq.set_device_id(req.register_().device_id());
    areq.set_display_name(req.register_().display_name());
    areq.set_password(req.register_().password());
    auth::RegisterResponse arsp;
    if (!GatewayAuthClients::Instance().AuthRegister(areq, &arsp) || !arsp.ok()) {
        body->set_ok(false);
        body->set_message(arsp.message().empty() ? "register failed" : arsp.message());
        rsp.set_ok(false);
        rsp.set_message(body->message());
        return EncodeResponse(rsp, response_frame);
    }
    body->set_ok(true);
    body->set_message("ok");
    body->set_player_id(arsp.player_id());
    rsp.set_ok(true);
    rsp.set_message("ok");
    return EncodeResponse(rsp, response_frame);
}

bool OrchestrateGatewayReconnect(const std::string &gateway_instance_id, uint64_t connection_id,
                                 const std::string &request_payload, std::string *response_frame,
                                 GatewayLoginRoute *route_out) {
    if (route_out)
        *route_out = GatewayLoginRoute{};
    game::GameRequest req;
    game::GameResponse rsp;
    if (!req.ParseFromString(request_payload) || !req.has_reconnect()) {
        rsp.set_ok(false);
        rsp.set_message("invalid reconnect payload");
        return EncodeResponse(rsp, response_frame);
    }
    rsp.set_seq(req.seq());
    auto *body = rsp.mutable_reconnect();

    if (!GatewayAuthClients::Instance().ready()) {
        body->set_ok(false);
        body->set_message("session client not ready");
        rsp.set_ok(false);
        rsp.set_message(body->message());
        return EncodeResponse(rsp, response_frame);
    }

    sess::PrepareReconnectRequest preq;
    preq.set_player_id(req.reconnect().player_id());
    preq.set_session_id(req.reconnect().session_id());
    preq.set_reconnect_ticket(req.reconnect().reconnect_ticket());
    preq.set_gateway_instance_id(gateway_instance_id);
    preq.set_last_server_seq(req.reconnect().last_server_seq());
    preq.set_operation_id("rec:" + gateway_instance_id + ":" + std::to_string(connection_id) + ":" +
                          req.reconnect().session_id() + ":" + req.reconnect().reconnect_ticket());
    sess::PrepareReconnectResponse prsp;
    if (!GatewayAuthClients::Instance().PrepareReconnect(preq, &prsp) || !prsp.ok()) {
        body->set_ok(false);
        body->set_message(prsp.message().empty() ? "prepare reconnect failed" : prsp.message());
        rsp.set_ok(false);
        rsp.set_message(body->message());
        return EncodeResponse(rsp, response_frame);
    }
    if (prsp.gamelogic_instance_id().empty()) {
        sess::AbortReconnectRequest areq;
        areq.set_player_id(req.reconnect().player_id());
        areq.set_session_id(prsp.session_id());
        areq.set_operation_id(preq.operation_id());
        areq.set_candidate_fence_token(prsp.candidate_fence_token());
        sess::AbortReconnectResponse arsp;
        GatewayAuthClients::Instance().AbortReconnect(areq, &arsp);
        body->set_ok(false);
        body->set_message("reconnect missing logic route");
        rsp.set_ok(false);
        rsp.set_message(body->message());
        return EncodeResponse(rsp, response_frame);
    }

    glrpc::BindPlayerRequest breq;
    breq.set_request_id(connection_id);
    breq.set_player_id(req.reconnect().player_id());
    breq.set_session_id(prsp.session_id());
    breq.set_fence_token(prsp.candidate_fence_token());
    breq.set_gamelogic_instance_id(prsp.gamelogic_instance_id());
    breq.set_map_instance_id(prsp.map_instance_id());
    breq.set_map_owner_epoch(prsp.map_owner_epoch());
    breq.set_route_version(prsp.route_version());
    breq.set_gateway_instance_id(gateway_instance_id);
    breq.set_generation(prsp.candidate_generation());
    breq.set_idempotency_key(prsp.session_id() + ":rebind");
    glrpc::BindPlayerResponse brsp;
    if (!GatewayAuthClients::Instance().BindPlayer(prsp.gamelogic_instance_id(), breq, &brsp) ||
        !brsp.ok()) {
        sess::AbortReconnectRequest areq;
        areq.set_player_id(req.reconnect().player_id());
        areq.set_session_id(prsp.session_id());
        areq.set_operation_id(preq.operation_id());
        areq.set_candidate_fence_token(prsp.candidate_fence_token());
        sess::AbortReconnectResponse arsp;
        GatewayAuthClients::Instance().AbortReconnect(areq, &arsp);
        body->set_ok(false);
        body->set_message(brsp.message().empty() ? "rebind failed" : brsp.message());
        rsp.set_ok(false);
        rsp.set_message(body->message());
        return EncodeResponse(rsp, response_frame);
    }

    sess::CommitReconnectRequest creq;
    creq.set_player_id(req.reconnect().player_id());
    creq.set_session_id(prsp.session_id());
    creq.set_operation_id(preq.operation_id());
    creq.set_candidate_fence_token(prsp.candidate_fence_token());
    creq.set_candidate_generation(prsp.candidate_generation());
    creq.set_gateway_instance_id(gateway_instance_id);
    creq.set_connection_id(connection_id);
    sess::CommitReconnectResponse crsp;
    if (!GatewayAuthClients::Instance().CommitReconnect(creq, &crsp) || !crsp.ok()) {
        glrpc::UnbindPlayerRequest ureq;
        ureq.set_player_id(req.reconnect().player_id());
        ureq.set_session_id(prsp.session_id());
        ureq.set_fence_token(prsp.candidate_fence_token());
        ureq.set_reason("commit_reconnect_failed");
        glrpc::UnbindPlayerResponse ursp;
        GatewayAuthClients::Instance().UnbindPlayer(prsp.gamelogic_instance_id(), ureq, &ursp);
        body->set_ok(false);
        body->set_message(crsp.message().empty() ? "commit reconnect failed" : crsp.message());
        rsp.set_ok(false);
        rsp.set_message(body->message());
        return EncodeResponse(rsp, response_frame);
    }

    if (SessionRpcClient::Instance().ready()) {
        SessionRpcClient::Instance().BindConnection(req.reconnect().player_id(), crsp.fence_token(),
                                                    gateway_instance_id, connection_id);
    }

    if (route_out) {
        route_out->player_id = req.reconnect().player_id();
        route_out->session_id = crsp.session_id();
        route_out->fence_token = crsp.fence_token();
        route_out->generation = crsp.generation();
        route_out->gamelogic_instance_id = crsp.gamelogic_instance_id();
        route_out->map_instance_id = crsp.map_instance_id();
        route_out->map_owner_epoch = crsp.map_owner_epoch();
        route_out->route_version = crsp.route_version();
        route_out->pending_push_payloads.clear();
        route_out->need_full_snapshot = false;
    }

    body->set_ok(true);
    body->set_message("reconnect ok");
    body->set_token(crsp.fence_token());
    body->set_session_id(crsp.session_id());
    body->set_generation(crsp.generation());

    // 可靠 Push 回放（跨 Gateway Redis）；缺口则 NeedFullSnapshot，禁止伪成功补发
    const uint64_t last_seq = req.reconnect().last_server_seq();
    bool need_snap = false;
    std::vector<PushReplayEntry> replay;
#ifdef WEBSERVER_ENABLE_REDIS
    const std::string &sid = crsp.session_id();
    if (PushReplayStore::Instance().Available()) {
        if (!PushReplayStore::Instance().ReplayAfter(req.reconnect().player_id(), sid, last_seq,
                                                     &replay, &need_snap)) {
            if (!need_snap)
                need_snap = true;
        }
    } else
#endif
    {
        if (!PushReplayCache::Instance().ReplayAfter(req.reconnect().player_id(), last_seq, &replay,
                                                     &need_snap) &&
            last_seq != 0) {
            need_snap = true;
        }
    }
    body->set_need_full_snapshot(need_snap);
    if (!need_snap && !replay.empty()) {
        body->set_replay_from_seq(replay.front().server_seq);
        if (route_out) {
            for (const auto &e : replay) {
                // 重连补发必须是 ServerPushEnvelope，保留原始 server_seq / message_type
                game::GameResponse env;
                env.set_ok(true);
                env.set_message("server_push");
                auto *p = env.mutable_server_push();
                p->set_server_seq(e.server_seq);
                p->set_message_type(e.message_type.empty() ? "replay" : e.message_type);
                p->set_payload(e.payload);
                p->set_reliable(e.reliable);
                p->set_coalescable(false);
                std::string body_bytes;
                if (env.SerializeToString(&body_bytes))
                    route_out->pending_push_payloads.push_back(std::move(body_bytes));
            }
        }
    } else {
        body->set_replay_from_seq(0);
    }

    // 缺口：真正生成并下发全量状态快照；导出失败不得发送 ok=true 伪快照
    if (need_snap) {
        glrpc::ExportPlayerSnapshotRequest er;
        er.set_player_id(req.reconnect().player_id());
        er.set_session_id(crsp.session_id());
        er.set_fence_token(crsp.fence_token());
        er.set_transfer_id("reconnect-full-snap");
        er.set_target_gamelogic_id(crsp.gamelogic_instance_id());
        glrpc::ExportPlayerSnapshotResponse xrsp;
        const bool export_ok =
            GatewayAuthClients::Instance().ExportPlayerSnapshot(crsp.gamelogic_instance_id(), er,
                                                                &xrsp) &&
            xrsp.ok() && xrsp.has_snapshot();
        if (!export_ok) {
            body->set_need_full_snapshot(true);
            body->set_message("reconnect ok; full_snapshot_export_failed_retry");
        } else if (route_out) {
            game::GameResponse snap_rsp;
            snap_rsp.set_ok(true);
            snap_rsp.set_message("full_snapshot");
            auto *fs = snap_rsp.mutable_full_snapshot();
            bool filled = false;
            if (!xrsp.public_full_snapshot().empty() &&
                fs->ParseFromString(xrsp.public_full_snapshot()) && fs->ok()) {
                filled = true;
            } else {
                fs->set_ok(true);
                fs->set_message("FULL_SNAPSHOT");
                fs->set_player_id(req.reconnect().player_id());
                fs->set_asset_version(xrsp.snapshot().state().asset_version());
                for (const auto &it : xrsp.snapshot().state().bag()) {
                    fs->add_item_ids(it.item_id());
                    fs->add_item_counts(it.count());
                }
                filled = fs->ok() && (fs->player_id() != 0);
            }
            if (!filled) {
                body->set_need_full_snapshot(true);
                body->set_message("reconnect ok; full_snapshot_empty_retry");
            } else {
            std::string snap_payload;
            uint64_t seq = 0;
            bool stored = true;
#ifdef WEBSERVER_ENABLE_REDIS
            if (PushReplayStore::Instance().Available()) {
                seq = PushReplayStore::Instance().ReserveSeq(req.reconnect().player_id(), sid);
                if (seq == 0) {
                    body->set_need_full_snapshot(true);
                    body->set_message("reconnect ok; full_snapshot_reserve_failed_retry");
                    stored = false;
                } else {
                    fs->set_baseline_server_seq(seq);
                    if (!snap_rsp.SerializeToString(&snap_payload)) {
                        body->set_need_full_snapshot(true);
                        body->set_message("reconnect ok; full_snapshot_encode_failed_retry");
                        stored = false;
                    } else if (!PushReplayStore::Instance().AppendReserved(
                                   req.reconnect().player_id(), sid, seq, "full_snapshot",
                                   snap_payload)) {
                        body->set_need_full_snapshot(true);
                        body->set_message("reconnect ok; full_snapshot_store_failed_retry");
                        stored = false;
                    }
                }
            } else if (!snap_rsp.SerializeToString(&snap_payload)) {
                body->set_need_full_snapshot(true);
                body->set_message("reconnect ok; full_snapshot_encode_failed_retry");
                stored = false;
            }
#else
            if (!snap_rsp.SerializeToString(&snap_payload)) {
                body->set_need_full_snapshot(true);
                body->set_message("reconnect ok; full_snapshot_encode_failed_retry");
                stored = false;
            }
#endif
            if (stored) {
                game::GameResponse env;
                env.set_ok(true);
                env.set_message("server_push");
                auto *p = env.mutable_server_push();
                p->set_server_seq(seq);
                p->set_message_type("full_snapshot");
                p->set_payload(snap_payload);
                p->set_reliable(true);
                std::string env_bytes;
                if (env.SerializeToString(&env_bytes))
                    route_out->pending_push_payloads.push_back(std::move(env_bytes));
                else
                    route_out->pending_push_payloads.push_back(std::move(snap_payload));
            }
            }
        }
    }

    if (route_out) {
        route_out->need_full_snapshot = need_snap;
        route_out->last_server_seq = last_seq;
    }

    rsp.set_ok(true);
    rsp.set_message(need_snap ? "reconnect ok need_full_snapshot" : "reconnect ok");
    LOG_INFO << "Gateway reconnect orchestrated player=" << req.reconnect().player_id()
             << " logic=" << crsp.gamelogic_instance_id() << " conn=" << connection_id
             << " need_snap=" << need_snap << " replay_n=" << replay.size();
    return EncodeResponse(rsp, response_frame);
}

bool OrchestrateGatewayLogout(const std::string &gateway_instance_id, uint64_t connection_id,
                              const std::string &request_payload, std::string *response_frame) {
    (void)gateway_instance_id;
    game::GameRequest req;
    game::GameResponse rsp;
    if (!req.ParseFromString(request_payload) || !req.has_logout()) {
        rsp.set_ok(false);
        rsp.set_message("invalid logout");
        return EncodeResponse(rsp, response_frame);
    }
    rsp.set_seq(req.seq());
    GatewayConnRegistry::Bind bind;
    const bool has_bind = GatewayConnRegistry::Instance().FindByConnection(connection_id, &bind);
    if (has_bind) {
        if (bind.gamelogic_instance_id.empty()) {
            rsp.set_ok(false);
            rsp.set_message("logout missing logic route");
            return EncodeResponse(rsp, response_frame);
        }
        glrpc::UnbindPlayerRequest ureq;
        ureq.set_player_id(bind.player_id != 0 ? bind.player_id : req.logout().player_id());
        ureq.set_session_id(bind.session_id);
        ureq.set_fence_token(bind.token.empty() ? req.logout().token() : bind.token);
        ureq.set_reason("logout");
        ureq.set_idempotency_key(bind.session_id + ":unbind");
        glrpc::UnbindPlayerResponse ursp;
        GatewayAuthClients::Instance().UnbindPlayer(bind.gamelogic_instance_id, ureq, &ursp);
    }
    game::LogoutRsp body;
    if (has_bind && GatewayAuthClients::Instance().ready()) {
        sess::LogoutRequest lreq;
        lreq.set_player_id(bind.player_id);
        lreq.set_session_id(bind.session_id);
        lreq.set_fence_token(bind.token);
        sess::LogoutResponse lrsp;
        if (GatewayAuthClients::Instance().LogoutV2(lreq, &lrsp)) {
            body.set_ok(lrsp.ok());
            body.set_message(lrsp.message());
        }
    } else if (SessionRpcClient::Instance().ready()) {
        SessionRpcClient::Instance().Logout(req.logout(), &body);
    } else {
        body.set_ok(false);
        body.set_message("session client not ready");
    }
    rsp.mutable_logout()->CopyFrom(body);
    rsp.set_ok(body.ok());
    rsp.set_message(body.message());
    GatewayConnRegistry::Instance().Forget(connection_id);
    return EncodeResponse(rsp, response_frame);
}

namespace {

template <typename Fn>
bool LaunchRpcOffload(uint64_t shard_key, Fn fn) {
    PlayerSerialQueue::Instance().MarkAsyncInFlight(shard_key);
    auto heap = std::make_shared<Fn>(std::move(fn));
    if (!RpcOffloadPool::Instance().TryPost([heap]() { (*heap)(); })) {
        PlayerSerialQueue::Instance().ClearAsyncInFlight(shard_key);
        return false;
    }
    return true;
}

}  // namespace

bool BeginOrchestrateGatewayLogin(const std::string &gateway_instance_id, uint64_t connection_id,
                                  const std::string &request_payload, uint64_t shard_key,
                                  GatewayAuthDone done) {
    if (!done)
        return false;
    return LaunchRpcOffload(shard_key, [gateway_instance_id, connection_id, request_payload,
                                        shard_key, done = std::move(done)]() mutable {
        std::string out;
        GatewayLoginRoute route;
        const bool ok =
            OrchestrateGatewayLogin(gateway_instance_id, connection_id, request_payload, &out,
                                    &route);
        if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(
                shard_key, [done = std::move(done), ok, out = std::move(out),
                            route = std::move(route)]() mutable {
                    done(ok, std::move(out), std::move(route));
                })) {
            if (ok && route.player_id != 0)
                CompensateGatewaySession(route.player_id, route.session_id, route.fence_token,
                                        route.generation);
        }
    });
}

bool BeginOrchestrateGatewayRegister(const std::string &request_payload, uint64_t shard_key,
                                     GatewayAuthDone done) {
    if (!done)
        return false;
    return LaunchRpcOffload(shard_key, [request_payload, shard_key, done = std::move(done)]() mutable {
        std::string out;
        const bool ok = OrchestrateGatewayRegister(request_payload, &out);
        if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(
                shard_key, [done = std::move(done), ok, out = std::move(out)]() mutable {
                    done(ok, std::move(out), GatewayLoginRoute{});
                })) {
            (void)ok;
        }
    });
}

bool BeginOrchestrateGatewayReconnect(const std::string &gateway_instance_id, uint64_t connection_id,
                                      const std::string &request_payload, uint64_t shard_key,
                                      GatewayAuthDone done) {
    if (!done)
        return false;
    return LaunchRpcOffload(shard_key, [gateway_instance_id, connection_id, request_payload,
                                        shard_key, done = std::move(done)]() mutable {
        std::string out;
        GatewayLoginRoute route;
        const bool ok = OrchestrateGatewayReconnect(gateway_instance_id, connection_id,
                                                    request_payload, &out, &route);
        if (!PlayerSerialQueue::Instance().CompleteAsyncInFlight(
                shard_key, [done = std::move(done), ok, out = std::move(out),
                            route = std::move(route)]() mutable {
                    done(ok, std::move(out), std::move(route));
                })) {
            if (ok && route.player_id != 0)
                CompensateGatewaySession(route.player_id, route.session_id, route.fence_token,
                                        route.generation);
        }
    });
}

}  // namespace gameproto
