#include "SessionServiceImpl.h"

#include "GatewayPushClient.h"
#include "HealthyLogicOwners.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PlacementStore.h"
#include "SessionStore.h"

#include <brpc/controller.h>

#include <vector>

namespace {

void FillPlacementPb(const PlacementRecord &r, sess::PlacementRecord *pb) {
    if (!pb)
        return;
    pb->set_realm_id(r.realm_id);
    pb->set_map_template_id(r.map_template_id);
    pb->set_map_instance_id(r.map_instance_id);
    pb->set_owner_logic_server_id(r.owner_logic_server_id);
    pb->set_owner_epoch(r.owner_epoch);
    pb->set_route_version(r.route_version);
    pb->set_state(PlacementStore::StateToString(r.state));
    pb->set_updated_at(r.updated_at);
    pb->set_lease_until(r.lease_until);
}

}  // namespace

void SessionServiceImpl::AcquireSession(::google::protobuf::RpcController *controller,
                                        const ::sess::AcquireSessionRequest *request,
                                        ::sess::AcquireSessionResponse *response,
                                        ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    AcquireSessionInput in;
    in.account_id = request->account_id();
    in.player_id = request->player_id();
    in.device_id = request->device_id();
    in.server_id = request->server_id();
    in.ttl_sec = request->ttl_sec();
    in.kick_other_device = request->kick_other_device();
    in.gateway_instance_id = request->gateway_instance_id();
    in.preferred_gamelogic_instance_id = request->preferred_gamelogic_instance_id();
    in.operation_id = request->operation_id();
    RefreshHealthyLogicOwners();
    AcquireSessionResult out;
    SessionStore::Instance().AcquireSession(in, &out);
    response->set_ok(out.ok);
    response->set_message(out.message);
    response->set_error_code(out.error_code);
    response->set_session_id(out.session_id);
    response->set_fence_token(out.fence_token);
    response->set_generation(out.generation);
    response->set_gamelogic_instance_id(out.gamelogic_instance_id);
    response->set_map_instance_id(out.map_instance_id);
    response->set_map_owner_epoch(out.map_owner_epoch);
    response->set_route_version(out.route_version);
    response->set_kicked_previous(out.kicked_previous);
    response->set_login_time_sec(out.login_time_sec);
    response->set_server_id(out.server_id);
}

void SessionServiceImpl::MarkDisconnectedV2(::google::protobuf::RpcController *controller,
                                            const ::sess::MarkDisconnectedRequest *request,
                                            ::sess::MarkDisconnectedResponse *response,
                                            ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    const bool ok = SessionStore::Instance().MarkDisconnected(
        request->player_id(), request->fence_token(), request->generation());
    response->set_ok(ok);
    response->set_message(ok ? "disconnected" : "ignored");
}

void SessionServiceImpl::ReconnectV2(::google::protobuf::RpcController *controller,
                                     const ::sess::ReconnectRequest *request,
                                     ::sess::ReconnectResponse *response,
                                     ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    ReconnectSessionInput in;
    in.player_id = request->player_id();
    in.session_id = request->session_id();
    in.reconnect_ticket = request->reconnect_ticket();
    in.gateway_instance_id = request->gateway_instance_id();
    in.last_server_seq = request->last_server_seq();
    in.operation_id = request->operation_id();
    AcquireSessionResult out;
    SessionRecord route;
    const bool ok = SessionStore::Instance().ReconnectSession(in, &out, &route);
    response->set_ok(ok && out.ok);
    response->set_message(out.message);
    response->set_session_id(out.session_id);
    response->set_fence_token(out.fence_token);
    response->set_generation(out.generation);
    if (response->ok()) {
        response->set_gamelogic_instance_id(route.gamelogic_instance_id);
        response->set_map_instance_id(route.map_instance_id);
        response->set_map_owner_epoch(route.map_owner_epoch);
        response->set_route_version(route.route_version);
        if (!request->gateway_instance_id().empty()) {
            SessionStore::Instance().BindConnection(request->player_id(), out.fence_token,
                                                    request->gateway_instance_id(), 0);
        }
    }
}

void SessionServiceImpl::PrepareReconnect(::google::protobuf::RpcController *controller,
                                          const ::sess::PrepareReconnectRequest *request,
                                          ::sess::PrepareReconnectResponse *response,
                                          ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    ReconnectSessionInput in;
    in.player_id = request->player_id();
    in.session_id = request->session_id();
    in.reconnect_ticket = request->reconnect_ticket();
    in.gateway_instance_id = request->gateway_instance_id();
    in.last_server_seq = request->last_server_seq();
    in.operation_id = request->operation_id();
    AcquireSessionResult out;
    const bool ok = SessionStore::Instance().PrepareReconnect(in, &out);
    response->set_ok(ok && out.ok);
    response->set_message(out.message);
    response->set_error_code(out.error_code);
    response->set_session_id(out.session_id);
    response->set_candidate_fence_token(out.fence_token);
    response->set_candidate_generation(out.generation);
    response->set_gamelogic_instance_id(out.gamelogic_instance_id);
    response->set_map_instance_id(out.map_instance_id);
    response->set_map_owner_epoch(out.map_owner_epoch);
    response->set_route_version(out.route_version);
    response->set_operation_id(request->operation_id());
}

void SessionServiceImpl::CommitReconnect(::google::protobuf::RpcController *controller,
                                         const ::sess::CommitReconnectRequest *request,
                                         ::sess::CommitReconnectResponse *response,
                                         ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    AcquireSessionResult out;
    const bool ok = SessionStore::Instance().CommitReconnect(
        request->player_id(), request->operation_id(), request->candidate_fence_token(),
        request->candidate_generation(), request->gateway_instance_id(), request->connection_id(),
        &out);
    response->set_ok(ok && out.ok);
    response->set_message(out.message);
    response->set_error_code(out.error_code);
    response->set_session_id(out.session_id);
    response->set_fence_token(out.fence_token);
    response->set_generation(out.generation);
    response->set_gamelogic_instance_id(out.gamelogic_instance_id);
    response->set_map_instance_id(out.map_instance_id);
    response->set_map_owner_epoch(out.map_owner_epoch);
    response->set_route_version(out.route_version);
}

void SessionServiceImpl::AbortReconnect(::google::protobuf::RpcController *controller,
                                        const ::sess::AbortReconnectRequest *request,
                                        ::sess::AbortReconnectResponse *response,
                                        ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    const bool ok = SessionStore::Instance().AbortReconnect(
        request->player_id(), request->operation_id(), request->candidate_fence_token());
    response->set_ok(ok);
    response->set_message(ok ? "aborted" : "abort failed");
}

void SessionServiceImpl::GetSessionOperation(::google::protobuf::RpcController *controller,
                                             const ::sess::GetSessionOperationRequest *request,
                                             ::sess::GetSessionOperationResponse *response,
                                             ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    SessionOpStatus st = SessionOpStatus::NotFound;
    std::string kind;
    AcquireSessionResult out;
    if (!SessionStore::Instance().GetSessionOperation(request->operation_id(), &st, &kind, &out)) {
        response->set_ok(false);
        response->set_message("lookup failed");
        response->set_status("NOT_FOUND");
        return;
    }
    response->set_ok(true);
    if (st == SessionOpStatus::Pending) {
        response->set_status("PENDING");
        response->set_message("pending");
        return;
    }
    if (st == SessionOpStatus::NotFound) {
        response->set_status("NOT_FOUND");
        response->set_message("not found");
        return;
    }
    response->set_status("DONE");
    response->set_op_kind(kind);
    response->set_message("ok");
    response->set_result_ok(out.ok);
    response->set_result_message(out.message);
    response->set_error_code(out.error_code);
    response->set_session_id(out.session_id);
    response->set_fence_token(out.fence_token);
    response->set_generation(out.generation);
    response->set_gamelogic_instance_id(out.gamelogic_instance_id);
    response->set_map_instance_id(out.map_instance_id);
    response->set_map_owner_epoch(out.map_owner_epoch);
    response->set_route_version(out.route_version);
    response->set_kicked_previous(out.kicked_previous);
    response->set_login_time_sec(out.login_time_sec);
    response->set_server_id(out.server_id);
}

void SessionServiceImpl::LogoutV2(::google::protobuf::RpcController *controller,
                                  const ::sess::LogoutRequest *request,
                                  ::sess::LogoutResponse *response,
                                  ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    game::LogoutReq legacy;
    legacy.set_player_id(request->player_id());
    legacy.set_token(request->fence_token());
    game::LogoutRsp legacy_rsp;
    SessionStore::Instance().Logout(legacy, &legacy_rsp);
    response->set_ok(legacy_rsp.ok());
    response->set_message(legacy_rsp.message());
}

void SessionServiceImpl::Kick(::google::protobuf::RpcController *controller,
                              const ::sess::KickRequest *request, ::sess::KickResponse *response,
                              ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    SessionStore::KickResult kr;
    const bool ok = SessionStore::Instance().Kick(request->player_id(), request->reason(), &kr);
    response->set_ok(ok);
    response->set_message(ok ? "kicked" : (kr.message.empty() ? "kick failed" : kr.message));
    if (ok && !kr.old_gateway_id.empty()) {
        GatewayPushClient::Instance().KickConnectionAsync(
            kr.old_gateway_id, request->player_id(), kr.old_session_id, kr.old_generation,
            request->reason());
        OpsMetrics::Instance().IncKickGatewayAttempt();
    }
    LOG_INFO << "Kick player_id=" << request->player_id() << " ok=" << ok
             << " new_gen=" << kr.new_generation << " old_gw=" << kr.old_gateway_id
             << " reason=" << request->reason();
}

void SessionServiceImpl::ResolveOrCreateMap(::google::protobuf::RpcController *controller,
                                            const ::sess::ResolveOrCreateMapRequest *request,
                                            ::sess::ResolveOrCreateMapResponse *response,
                                            ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    RefreshHealthyLogicOwners();
    ResolveOrCreateInput in;
    in.realm_id = request->realm_id();
    in.map_template_id = request->map_template_id();
    in.map_instance_id = request->map_instance_id();
    in.preferred_owner = request->preferred_owner();
    in.force_new = request->force_new();
    in.player_id = request->player_id();
    in.operation_id = request->operation_id();
    in.capacity = request->public_map_capacity();
    ResolveOrCreateResult out;
    PlacementStore::Instance().ResolveOrCreate(in, &out);
    response->set_ok(out.ok);
    response->set_message(out.message);
    response->set_error_code(out.error_code);
    if (out.ok)
        FillPlacementPb(out.placement, response->mutable_placement());
}

void SessionServiceImpl::GetPlacement(::google::protobuf::RpcController *controller,
                                      const ::sess::GetPlacementRequest *request,
                                      ::sess::GetPlacementResponse *response,
                                      ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    PlacementRecord rec;
    const bool ok = PlacementStore::Instance().Get(request->map_instance_id(), &rec);
    response->set_ok(ok);
    response->set_message(ok ? "ok" : "not found");
    if (ok)
        FillPlacementPb(rec, response->mutable_placement());
}

void SessionServiceImpl::MigrateMap(::google::protobuf::RpcController *controller,
                                    const ::sess::MigrateMapRequest *request,
                                    ::sess::MigrateMapResponse *response,
                                    ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    PlacementRecord rec;
    std::string err;
    const bool ok = PlacementStore::Instance().Migrate(
        request->map_instance_id(), request->new_owner_logic_server_id(), request->expect_epoch(),
        request->idempotency_key(), &rec, &err);
    response->set_ok(ok);
    response->set_message(ok ? "ok" : err);
    response->set_error_code(ok ? "" : "MIGRATE_FAILED");
    if (ok)
        FillPlacementPb(rec, response->mutable_placement());
}

void SessionServiceImpl::MarkRecovering(::google::protobuf::RpcController *controller,
                                        const ::sess::MarkRecoveringRequest *request,
                                        ::sess::MarkRecoveringResponse *response,
                                        ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    PlacementRecord rec;
    const bool ok =
        PlacementStore::Instance().MarkRecovering(request->map_instance_id(), request->reason(), &rec);
    response->set_ok(ok);
    response->set_message(ok ? "recovering" : "failed");
    if (ok)
        FillPlacementPb(rec, response->mutable_placement());
}

void SessionServiceImpl::HeartbeatOwner(::google::protobuf::RpcController *controller,
                                        const ::sess::HeartbeatOwnerRequest *request,
                                        ::sess::HeartbeatOwnerResponse *response,
                                        ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    int64_t lease_until = 0;
    const bool ok = PlacementStore::Instance().Heartbeat(
        request->map_instance_id(), request->owner_logic_server_id(), request->owner_epoch(),
        request->lease_sec(), &lease_until);
    response->set_ok(ok);
    response->set_message(ok ? "ok" : "rejected");
    response->set_lease_until(lease_until);
}

void SessionServiceImpl::UpdatePlayerRoute(::google::protobuf::RpcController *controller,
                                           const ::sess::UpdatePlayerRouteRequest *request,
                                           ::sess::UpdatePlayerRouteResponse *response,
                                           ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    uint64_t rv = 0;
    std::string err;
    const bool ok = SessionStore::Instance().UpdatePlayerRoute(
        request->player_id(), request->fence_token(), request->gamelogic_instance_id(),
        request->map_instance_id(), request->map_owner_epoch(), request->route_version(),
        request->gateway_instance_id(), request->push_endpoint(), &rv, &err);
    response->set_ok(ok);
    response->set_message(ok ? "ok" : err);
    response->set_route_version(rv);
}

void SessionServiceImpl::BeginPlayerTransfer(::google::protobuf::RpcController *controller,
                                             const ::sess::BeginPlayerTransferRequest *request,
                                             ::sess::BeginPlayerTransferResponse *response,
                                             ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    SessionStore::TransferBeginIn in;
    in.player_id = request->player_id();
    in.fence_token = request->fence_token();
    in.expected_route_version = request->expected_route_version();
    in.from_logic = request->from_gamelogic_instance_id();
    in.to_logic = request->to_gamelogic_instance_id();
    in.map_instance_id = request->map_instance_id();
    in.map_owner_epoch = request->map_owner_epoch();
    in.transfer_id = request->transfer_id();
    in.gateway_instance_id = request->gateway_instance_id();
    SessionStore::TransferBeginOut out;
    SessionStore::Instance().BeginPlayerTransfer(in, &out);
    response->set_ok(out.ok);
    response->set_message(out.message);
    response->set_error_code(out.error_code);
    response->set_transfer_id(out.transfer_id);
    response->set_route_version(out.route_version);
    response->set_route_state(out.route_state);
}

void SessionServiceImpl::CommitPlayerTransfer(::google::protobuf::RpcController *controller,
                                              const ::sess::CommitPlayerTransferRequest *request,
                                              ::sess::CommitPlayerTransferResponse *response,
                                              ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    SessionStore::TransferCommitIn in;
    in.player_id = request->player_id();
    in.fence_token = request->fence_token();
    in.transfer_id = request->transfer_id();
    in.to_logic = request->to_gamelogic_instance_id();
    in.map_instance_id = request->map_instance_id();
    in.map_owner_epoch = request->map_owner_epoch();
    in.gateway_instance_id = request->gateway_instance_id();
    SessionStore::TransferCommitOut out;
    SessionStore::Instance().CommitPlayerTransfer(in, &out);
    response->set_ok(out.ok);
    response->set_message(out.message);
    response->set_error_code(out.error_code);
    response->set_route_version(out.route_version);
    response->set_gamelogic_instance_id(out.gamelogic_instance_id);
    response->set_map_instance_id(out.map_instance_id);
    response->set_map_owner_epoch(out.map_owner_epoch);
    response->set_route_state(out.route_state);
}

void SessionServiceImpl::AbortPlayerTransfer(::google::protobuf::RpcController *controller,
                                             const ::sess::AbortPlayerTransferRequest *request,
                                             ::sess::AbortPlayerTransferResponse *response,
                                             ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    uint64_t rv = 0;
    std::string err;
    const bool ok = SessionStore::Instance().AbortPlayerTransfer(
        request->player_id(), request->fence_token(), request->transfer_id(), &err, &rv);
    response->set_ok(ok);
    response->set_message(ok ? "ok" : err);
    response->set_error_code(ok ? "OK" : "ABORT_FAILED");
    response->set_route_version(rv);
    response->set_route_state(ok ? "ONLINE" : "");
}

void SessionServiceImpl::GetPlayerRoute(::google::protobuf::RpcController *controller,
                                        const ::sess::GetPlayerRouteRequest *request,
                                        ::sess::GetPlayerRouteResponse *response,
                                        ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    SessionRecord rec;
    std::string state, tid, err;
    if (!SessionStore::Instance().GetPlayerRoute(request->player_id(), request->fence_token(), &rec,
                                                 &state, &tid, &err)) {
        response->set_ok(false);
        response->set_message(err);
        return;
    }
    response->set_ok(true);
    response->set_message("ok");
    response->set_session_id(rec.session_id);
    response->set_fence_token(rec.token);
    response->set_generation(rec.generation);
    response->set_gateway_instance_id(rec.gateway_id);
    response->set_gamelogic_instance_id(rec.gamelogic_instance_id);
    response->set_map_instance_id(rec.map_instance_id);
    response->set_map_owner_epoch(rec.map_owner_epoch);
    response->set_route_version(rec.route_version);
    response->set_route_state(state.empty() ? "ONLINE" : state);
    response->set_transfer_id(tid);
}

void SessionServiceImpl::Login(::google::protobuf::RpcController *controller,
                               const ::game::LoginReq *request, ::game::LoginRsp *response,
                               ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    // 兼容旧调用；正确路径为 Gateway→Auth→AcquireSession
    SessionStore::Instance().Login(*request, response);
}

void SessionServiceImpl::Reconnect(::google::protobuf::RpcController *controller,
                                   const ::game::ReconnectReq *request,
                                   ::game::ReconnectRsp *response,
                                   ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    SessionStore::Instance().Reconnect(*request, response);
}

void SessionServiceImpl::Logout(::google::protobuf::RpcController *controller,
                                const ::game::LogoutReq *request, ::game::LogoutRsp *response,
                                ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    SessionStore::Instance().Logout(*request, response);
}

void SessionServiceImpl::ValidateToken(::google::protobuf::RpcController *controller,
                                       const ::sess::ValidateTokenReq *request,
                                       ::sess::ValidateTokenRsp *response,
                                       ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    std::string err;
    const bool ok =
        SessionStore::Instance().ValidateToken(request->player_id(), request->token(), &err);
    response->set_ok(ok);
    response->set_message(ok ? "ok" : err);
}

void SessionServiceImpl::BindConnection(::google::protobuf::RpcController *controller,
                                        const ::sess::BindConnectionReq *request,
                                        ::sess::BindConnectionRsp *response,
                                        ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    const bool ok = SessionStore::Instance().BindConnection(
        request->player_id(), request->token(), request->gateway_id(), request->connection_id());
    response->set_ok(ok);
    response->set_message(ok ? "ok" : "bind failed");
}

void SessionServiceImpl::MarkDisconnected(::google::protobuf::RpcController *controller,
                                          const ::sess::MarkDisconnectedReq *request,
                                          ::sess::MarkDisconnectedRsp *response,
                                          ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    const bool ok = SessionStore::Instance().MarkDisconnected(
        request->player_id(), request->token(), request->generation());
    response->set_ok(ok);
    response->set_message(ok ? "ok" : "ignored");
}
