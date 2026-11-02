#include "MessageRoute.h"

#include "CommandSpec.h"

namespace gameproto {

bool IsMailBoundRequest(const game::GameRequest &req) {
    const CommandSpec *s = FindCommandSpec(req.body_case());
    return s && s->mail;
}

bool IsGlobalServiceRequest(const game::GameRequest &req) {
    const CommandSpec *s = FindCommandSpec(req.body_case());
    return s && s->global_service;
}

bool IsLogicBoundRequest(const game::GameRequest &req) {
    return !IsGlobalServiceRequest(req);
}

}  // namespace gameproto
