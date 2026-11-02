#include "CommandPolicy.h"

#include "CommandSpec.h"
#include "FormalMode.h"

#include <cstdlib>
#include <cstring>

namespace gameproto {

bool AllowUnsafeDebugCommands() {
    if (FormalModeEnabled())
        return false;
    const char *v = std::getenv("GAMEMESH_ALLOW_UNSAFE_DEBUG_COMMANDS");
    if (!v || !*v)
        return false;
    return std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
           std::strcmp(v, "TRUE") == 0 || std::strcmp(v, "on") == 0;
}

namespace {

bool IsDangerousClientCommand(game::GameRequest::BodyCase body) {
    const CommandSpec *s = FindCommandSpec(body);
    return s && s->dangerous;
}

bool IsAuthenticatedClientAllowlisted(game::GameRequest::BodyCase body) {
    const CommandSpec *s = FindCommandSpec(body);
    return s && s->authenticated_allow;
}

}  // namespace

CommandDecision ValidateCommandPolicy(game::GameRequest::BodyCase body, CommandTrustLevel trust,
                                      bool formal_mode, std::string *err_code) {
    if (body == game::GameRequest::BODY_NOT_SET) {
        if (err_code)
            *err_code = "ERR_COMMAND_FORBIDDEN";
        return CommandDecision::Forbid;
    }
    if (trust == CommandTrustLevel::InternalService || trust == CommandTrustLevel::AdminService)
        return CommandDecision::Allow;

    if (trust == CommandTrustLevel::PreAuthClient) {
        const CommandSpec *s = FindCommandSpec(body);
        if (s && s->preauth_allow)
            return CommandDecision::Allow;
        {
            if (err_code)
                *err_code = "ERR_COMMAND_FORBIDDEN";
            return CommandDecision::Forbid;
        }
    }

    // AuthenticatedClient
    if (IsDangerousClientCommand(body)) {
        if (formal_mode || !AllowUnsafeDebugCommands()) {
            if (err_code)
                *err_code = "ERR_COMMAND_FORBIDDEN";
            return CommandDecision::Forbid;
        }
        return CommandDecision::Allow;
    }
    if (!IsAuthenticatedClientAllowlisted(body)) {
        if (err_code)
            *err_code = "ERR_COMMAND_FORBIDDEN";
        return CommandDecision::Forbid;
    }
    (void)formal_mode;
    return CommandDecision::Allow;
}

bool AllowClientTcpPayload(const std::string &payload, bool bound, bool formal_mode,
                           std::string *err_code) {
    game::GameRequest req;
    if (!req.ParseFromString(payload)) {
        if (err_code)
            *err_code = "ERR_BAD_FRAME";
        return false;
    }
    const CommandTrustLevel trust =
        bound ? CommandTrustLevel::AuthenticatedClient : CommandTrustLevel::PreAuthClient;
    return ValidateCommandPolicy(req.body_case(), trust, formal_mode, err_code) ==
           CommandDecision::Allow;
}

}  // namespace gameproto
