/**
 * Reconnect Prepare/Commit/Abort：Bind 前不得写 ONLINE；Abort 保留旧 ticket。
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
    SessionStore::Instance().SetLogicInstanceIds({"gl-0"});
    const uint64_t pid = 931000000ull + static_cast<uint64_t>(::getpid());
    game::LogoutReq lo;
    lo.set_player_id(pid);
    game::LogoutRsp lorsp;
    SessionStore::Instance().Logout(lo, &lorsp);

    AcquireSessionInput ain;
    ain.player_id = pid;
    ain.device_id = "rec-tx";
    ain.server_id = 1;
    ain.kick_other_device = true;
    AcquireSessionResult aout;
    if (!SessionStore::Instance().AcquireSession(ain, &aout) || !aout.ok) {
        std::printf("FAIL acquire %s\n", aout.message.c_str());
        return 1;
    }
    const std::string old_fence = aout.fence_token;
    const std::string sid = aout.session_id;
    const uint64_t old_gen = aout.generation;
    if (!SessionStore::Instance().MarkDisconnected(pid, old_fence, old_gen)) {
        std::printf("FAIL disconnect\n");
        return 1;
    }

    ReconnectSessionInput pin;
    pin.player_id = pid;
    pin.session_id = sid;
    pin.reconnect_ticket = old_fence;
    pin.operation_id = "op-prepare-1";
    AcquireSessionResult pout;
    if (!SessionStore::Instance().PrepareReconnect(pin, &pout) || !pout.ok) {
        std::printf("FAIL prepare %s\n", pout.message.c_str());
        return 1;
    }
    if (SessionStore::Instance().IsPlayerOnline(pid)) {
        std::printf("FAIL prepare wrote ONLINE\n");
        return 1;
    }
    std::string err;
    if (SessionStore::Instance().ValidateToken(pid, pout.fence_token, &err)) {
        std::printf("FAIL candidate fence valid before commit\n");
        return 1;
    }

    AcquireSessionResult p2;
    if (!SessionStore::Instance().PrepareReconnect(pin, &p2) || !p2.ok ||
        p2.fence_token != pout.fence_token || p2.generation != pout.generation) {
        std::printf("FAIL prepare idempotent\n");
        return 1;
    }

    if (!SessionStore::Instance().AbortReconnect(pid, pin.operation_id, pout.fence_token)) {
        std::printf("FAIL abort\n");
        return 1;
    }
    if (SessionStore::Instance().IsPlayerOnline(pid)) {
        std::printf("FAIL abort made ONLINE\n");
        return 1;
    }

    pin.operation_id = "op-prepare-2";
    AcquireSessionResult p3;
    if (!SessionStore::Instance().PrepareReconnect(pin, &p3) || !p3.ok) {
        std::printf("FAIL prepare after abort %s\n", p3.message.c_str());
        return 1;
    }
    AcquireSessionResult cout;
    if (!SessionStore::Instance().CommitReconnect(pid, pin.operation_id, p3.fence_token,
                                                  p3.generation, "gw-1", 42, &cout) ||
        !cout.ok) {
        std::printf("FAIL commit %s\n", cout.message.c_str());
        return 1;
    }
    if (!SessionStore::Instance().IsPlayerOnline(pid)) {
        std::printf("FAIL commit not ONLINE\n");
        return 1;
    }
    if (!SessionStore::Instance().ValidateToken(pid, cout.fence_token, &err)) {
        std::printf("FAIL new fence %s\n", err.c_str());
        return 1;
    }
    if (SessionStore::Instance().ValidateToken(pid, old_fence, &err)) {
        std::printf("FAIL old ticket still valid\n");
        return 1;
    }

    AcquireSessionResult stale;
    if (SessionStore::Instance().CommitReconnect(pid, pin.operation_id, p3.fence_token,
                                                 p3.generation, "gw-1", 42, &stale) &&
        stale.ok) {
        std::printf("FAIL second commit succeeded\n");
        return 1;
    }

    std::printf("OK reconnect_transaction_test\n");
    return 0;
}
