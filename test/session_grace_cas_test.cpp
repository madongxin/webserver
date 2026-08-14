/**
 * 宽限期过期必须 Redis CAS：Reconnect 成功后不得删掉新 Session。
 */
#include "Logging.h"
#include "SessionStore.h"
#include "game.pb.h"

#include <cstdio>
#include <string>
#include <unistd.h>

int main() {
    Logger::setLogLevel(Logger::WARN);
    if (!SessionStore::Instance().InitFromConfig()) {
        std::printf("FAIL redis unavailable\n");
        return 1;
    }
    SessionStore::Instance().SetLogicInstanceIds({"gl-0", "gl-1"});
    const uint64_t pid = 930000000ull + static_cast<uint64_t>(::getpid());
    game::LogoutReq lo;
    lo.set_player_id(pid);
    game::LogoutRsp lorsp;
    SessionStore::Instance().Logout(lo, &lorsp);

    AcquireSessionInput in;
    in.player_id = pid;
    in.device_id = "grace-cas";
    in.server_id = 1;
    in.kick_other_device = true;
    AcquireSessionResult out;
    if (!SessionStore::Instance().AcquireSession(in, &out) || !out.ok) {
        std::printf("FAIL acquire %s\n", out.message.c_str());
        return 1;
    }
    const std::string old_fence = out.fence_token;
    const std::string sid = out.session_id;
    const uint64_t old_gen = out.generation;
    if (!SessionStore::Instance().MarkDisconnected(pid, old_fence, old_gen)) {
        std::printf("FAIL mark disconnected\n");
        return 1;
    }

    std::string new_fence;
    uint64_t new_gen = 0;
    SessionStore::Instance().SetAfterGraceLoadHookForTest([&]() {
        ReconnectSessionInput rin;
        rin.player_id = pid;
        rin.session_id = sid;
        rin.reconnect_ticket = old_fence;
        rin.operation_id = std::string("grace-race:") + std::to_string(pid);
        AcquireSessionResult rout;
        SessionRecord route;
        if (SessionStore::Instance().ReconnectSession(rin, &rout, &route) && rout.ok) {
            new_fence = rout.fence_token;
            new_gen = rout.generation;
        }
    });
    SessionStore::Instance().ForceExpireGraceFromStaleLocalForTest(pid);
    SessionStore::Instance().SetAfterGraceLoadHookForTest(nullptr);
    if (!SessionStore::Instance().IsPlayerOnline(pid)) {
        std::printf("FAIL session deleted by stale grace expire\n");
        return 1;
    }
    if (new_fence.empty() || new_gen <= old_gen) {
        std::printf("FAIL reconnect in hook\n");
        return 1;
    }
    std::string err;
    if (!SessionStore::Instance().ValidateToken(pid, new_fence, &err)) {
        std::printf("FAIL new fence %s\n", err.c_str());
        return 1;
    }
    if (SessionStore::Instance().ValidateToken(pid, old_fence, &err)) {
        std::printf("FAIL old fence still valid\n");
        return 1;
    }
    std::printf("OK session_grace_cas_test\n");
    return 0;
}
