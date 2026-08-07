#include "SessionServiceImpl.h"

#include "Logging.h"
#include "SessionStore.h"

#include <brpc/controller.h>

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
    game::ReconnectReq legacy;
    legacy.set_player_id(request->player_id());
    legacy.set_session_id(request->session_id());
    legacy.set_reconnect_ticket(request->reconnect_ticket());
    legacy.set_last_server_seq(request->last_server_seq());
    game::ReconnectRsp legacy_rsp;
    SessionStore::Instance().Reconnect(legacy, &legacy_rsp);
    response->set_ok(legacy_rsp.ok());
    response->set_message(legacy_rsp.message());
    response->set_session_id(legacy_rsp.session_id());
    response->set_fence_token(legacy_rsp.token());
    response->set_generation(legacy_rsp.generation());
    if (legacy_rsp.ok() && !request->gateway_instance_id().empty()) {
        SessionStore::Instance().BindConnection(request->player_id(), legacy_rsp.token(),
                                                request->gateway_instance_id(), 0);
    }
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
    response->set_ok(false);
    response->set_message(std::string("kick deferred: ") + request->reason());
    LOG_WARN << "Kick stub player_id=" << request->player_id() << " reason=" << request->reason();
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
