/**
 * 阶段一：公网命令策略 — Formal 封闭 GrantItem/MailDeliver
 */
#include "CommandPolicy.h"
#include "FormalMode.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

}  // namespace

int main() {
    using gameproto::CommandDecision;
    using gameproto::CommandTrustLevel;
    using gameproto::ValidateCommandPolicy;

    std::string code;
    Expect(ValidateCommandPolicy(game::GameRequest::kGrantItem,
                                 CommandTrustLevel::AuthenticatedClient, true, &code) ==
                   CommandDecision::Forbid &&
               code == "ERR_COMMAND_FORBIDDEN",
           "formal grant forbidden");
    Expect(ValidateCommandPolicy(game::GameRequest::kMailDeliver,
                                 CommandTrustLevel::AuthenticatedClient, true, &code) ==
               CommandDecision::Forbid,
           "formal mail deliver forbidden");
    Expect(ValidateCommandPolicy(game::GameRequest::kPlayerMailSend,
                                 CommandTrustLevel::AuthenticatedClient, true, &code) ==
               CommandDecision::Allow,
           "formal player mail send allowed");
    Expect(ValidateCommandPolicy(game::GameRequest::kGrantItem, CommandTrustLevel::InternalService,
                                 true, &code) == CommandDecision::Allow,
           "internal grant allowed");
    Expect(ValidateCommandPolicy(game::GameRequest::kConsumeItem,
                                 CommandTrustLevel::AuthenticatedClient, true, &code) ==
               CommandDecision::Allow,
           "consume allowlisted");
    Expect(ValidateCommandPolicy(game::GameRequest::kFlushBag,
                                 CommandTrustLevel::AuthenticatedClient, false, &code) ==
               CommandDecision::Forbid,
           "unregistered/dangerous default forbid");
    Expect(ValidateCommandPolicy(game::GameRequest::kLogin, CommandTrustLevel::PreAuthClient, true,
                                 &code) == CommandDecision::Allow,
           "preauth login");

    ::unsetenv("GAMEMESH_ALLOW_UNSAFE_DEBUG_COMMANDS");
    ::unsetenv("GAMEMESH_FORMAL");
    Expect(!gameproto::AllowUnsafeDebugCommands(), "unsafe default off");

    ::setenv("GAMEMESH_ALLOW_UNSAFE_DEBUG_COMMANDS", "1", 1);
    Expect(gameproto::AllowUnsafeDebugCommands(), "unsafe on when non-formal");
    Expect(ValidateCommandPolicy(game::GameRequest::kGrantItem,
                                 CommandTrustLevel::AuthenticatedClient, false, &code) ==
               CommandDecision::Allow,
           "dev grant when unsafe");
    ::setenv("GAMEMESH_FORMAL", "1", 1);
    Expect(!gameproto::AllowUnsafeDebugCommands(), "formal forces unsafe off");
    Expect(ValidateCommandPolicy(game::GameRequest::kGrantItem,
                                 CommandTrustLevel::AuthenticatedClient, true, &code) ==
               CommandDecision::Forbid,
           "formal still forbids with env mis-set");
    ::unsetenv("GAMEMESH_FORMAL");
    ::unsetenv("GAMEMESH_ALLOW_UNSAFE_DEBUG_COMMANDS");

    game::GameRequest req;
    req.mutable_grant_item()->set_player_id(1);
    req.mutable_grant_item()->set_item_id(2);
    req.mutable_grant_item()->set_count(1);
    std::string payload;
    Expect(req.SerializeToString(&payload), "serialize");
    Expect(!gameproto::AllowClientTcpPayload(payload, true, true, &code),
           "tcp formal grant blocked");

    if (fails) {
        std::printf("command_policy_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK command_policy_test\n");
    return 0;
}
