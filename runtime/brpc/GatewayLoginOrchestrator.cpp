#include "GatewayLoginOrchestrator.h"

#include "GatewayAuthClients.h"
#include "GatewayConnRegistry.h"
#include "Logging.h"
#include "ProtoFraming.h"
#include "SessionRpcClient.h"
#include "game.pb.h"

namespace gameproto {

bool IsGatewayOwnedAuthPayload(const std::string &payload) {
    game::GameRequest req;
    if (!req.ParseFromString(payload))
        return false;
    return req.body_case() == game::GameRequest::kLogin ||
           req.body_case() == game::GameRequest::kLogout;
}

static bool EncodeResponse(const game::GameResponse &rsp, std::string *frame) {
    std::string body;
    if (!rsp.SerializeToString(&body))
        return false;
    return EncodeFrame(body, frame);
}

bool OrchestrateGatewayLogin(const std::string &gateway_instance_id, int connection_id,
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
    sess::AcquireSessionResponse srsp;
    if (!GatewayAuthClients::Instance().AcquireSession(sreq, &srsp) || !srsp.ok()) {
        login_body->set_ok(false);
        login_body->set_message(srsp.message().empty() ? "acquire session failed" : srsp.message());
        rsp.set_ok(false);
        rsp.set_message(login_body->message());
        return EncodeResponse(rsp, response_frame);
    }

    glrpc::BindPlayerRequest breq;
    breq.set_player_id(arsp.player_id());
    breq.set_session_id(srsp.session_id());
    breq.set_fence_token(srsp.fence_token());
    breq.set_gamelogic_instance_id(srsp.gamelogic_instance_id());
    breq.set_map_instance_id(srsp.map_instance_id());
    breq.set_map_owner_epoch(srsp.map_owner_epoch());
    breq.set_route_version(srsp.route_version());
    breq.set_gateway_instance_id(gateway_instance_id);
    breq.set_idempotency_key(srsp.session_id() + ":bind");
    glrpc::BindPlayerResponse brsp;
    if (!GatewayAuthClients::Instance().BindPlayer(srsp.gamelogic_instance_id(), breq, &brsp) ||
        !brsp.ok()) {
        if (SessionRpcClient::Instance().ready()) {
            game::LogoutReq legacy;
            legacy.set_player_id(arsp.player_id());
            legacy.set_token(srsp.fence_token());
            game::LogoutRsp lr;
            SessionRpcClient::Instance().Logout(legacy, &lr);
        }
        login_body->set_ok(false);
        login_body->set_message(brsp.message().empty() ? "bind player failed" : brsp.message());
        rsp.set_ok(false);
        rsp.set_message(login_body->message());
        return EncodeResponse(rsp, response_frame);
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
    rsp.set_ok(true);
    rsp.set_message("login ok");
    LOG_INFO << "Gateway login orchestrated player=" << arsp.player_id()
             << " logic=" << srsp.gamelogic_instance_id() << " conn=" << connection_id;
    return EncodeResponse(rsp, response_frame);
}

bool OrchestrateGatewayLogout(const std::string &gateway_instance_id, int connection_id,
                              const std::string &request_payload, std::string *response_frame) {
    (void)gateway_instance_id;
    (void)connection_id;
    game::GameRequest req;
    game::GameResponse rsp;
    if (!req.ParseFromString(request_payload) || !req.has_logout()) {
        rsp.set_ok(false);
        rsp.set_message("invalid logout");
        return EncodeResponse(rsp, response_frame);
    }
    rsp.set_seq(req.seq());
    GatewayConnRegistry::Bind bind;
    std::string logic_id = "gl-0";
    if (GatewayConnRegistry::Instance().FindByConnection(connection_id, &bind)) {
        logic_id = bind.gamelogic_instance_id.empty() ? "gl-0" : bind.gamelogic_instance_id;
        glrpc::UnbindPlayerRequest ureq;
        ureq.set_player_id(req.logout().player_id());
        ureq.set_session_id(bind.session_id);
        ureq.set_fence_token(req.logout().token());
        ureq.set_reason("logout");
        glrpc::UnbindPlayerResponse ursp;
        GatewayAuthClients::Instance().UnbindPlayer(logic_id, ureq, &ursp);
    }
    game::LogoutRsp body;
    if (SessionRpcClient::Instance().ready()) {
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

}  // namespace gameproto
