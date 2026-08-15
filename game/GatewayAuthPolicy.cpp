#include "GatewayAuthPolicy.h"

#include "game.pb.h"

namespace gameproto {

bool IsGatewayOwnedAuthPayload(const std::string &payload) {
    game::GameRequest req;
    if (!req.ParseFromString(payload))
        return false;
    return req.body_case() == game::GameRequest::kLogin ||
           req.body_case() == game::GameRequest::kLogout ||
           req.body_case() == game::GameRequest::kReconnect ||
           req.body_case() == game::GameRequest::kRegister;
}

bool IsPreAuthWhitelistPayload(const std::string &payload) {
    game::GameRequest req;
    if (!req.ParseFromString(payload))
        return false;
    switch (req.body_case()) {
    case game::GameRequest::kLogin:
    case game::GameRequest::kRegister:
    case game::GameRequest::kReconnect:
    case game::GameRequest::kClientHello:
    case game::GameRequest::kHeartbeat:
        return true;
    default:
        return false;
    }
}

}  // namespace gameproto
