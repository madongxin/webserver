/**
 * 阶段 3：Session 状态机 / 顶号 fence / 重连（需 Redis）
 */
#include "SessionStore.h"
#include "game.pb.h"

#include <cstdio>
#include <string>

namespace {

int Fail(const char *msg) {
    std::printf("FAIL %s\n", msg);
    return 1;
}

}  // namespace

int main() {
    if (!SessionStore::Instance().InitFromConfig()) {
        std::printf("SKIP session_store_test (Redis unavailable)\n");
        return 0;
    }

    const uint64_t pid = 900001;
    // 清理
    game::LogoutReq lo;
    lo.set_player_id(pid);
    game::LogoutRsp lorsp;
    SessionStore::Instance().Logout(lo, &lorsp);

    game::LoginReq login;
    login.set_player_id(pid);
    login.set_device_id("dev-a");
    login.set_server_id(1);
    login.set_kick_other_device(true);
    game::LoginRsp lrsp;
    if (!SessionStore::Instance().Login(login, &lrsp) || !lrsp.ok())
        return Fail("login1");
    const std::string token1 = lrsp.token();
    const std::string sid = lrsp.session_id();
    const uint64_t gen1 = lrsp.generation();
    if (token1.empty() || sid.empty() || gen1 == 0)
        return Fail("login fields");

    std::string err;
    if (!SessionStore::Instance().ValidateToken(pid, token1, &err))
        return Fail("validate online");

    // 顶号：再登录轮换 fence
    login.set_device_id("dev-b");
    game::LoginRsp lrsp2;
    if (!SessionStore::Instance().Login(login, &lrsp2) || !lrsp2.ok())
        return Fail("login2 replace");
    if (lrsp2.token() == token1)
        return Fail("token not rotated");
    if (!SessionStore::Instance().ValidateToken(pid, token1, &err)) {
        // old token should fail — good
    } else {
        return Fail("old token still valid");
    }
    if (!SessionStore::Instance().ValidateToken(pid, lrsp2.token(), &err))
        return Fail("new token invalid");

    const std::string token2 = lrsp2.token();
    const uint64_t gen2 = lrsp2.generation();
    if (gen2 <= gen1)
        return Fail("generation not increased");

    // 断线宽限
    if (!SessionStore::Instance().MarkDisconnected(pid, token2, gen2))
        return Fail("mark disconnected");
    if (SessionStore::Instance().ValidateToken(pid, token2, &err))
        return Fail("token valid while DISCONNECTED");

    // 旧 generation 断线回调应忽略（先重连再伪造旧回调）
    game::ReconnectReq rreq;
    rreq.set_player_id(pid);
    rreq.set_session_id(lrsp2.session_id().empty() ? sid : lrsp2.session_id());
    // session_id 在第二次 login 已换新
    rreq.set_session_id(lrsp2.session_id());
    rreq.set_reconnect_ticket(token2);
    game::ReconnectRsp rrsp;
    if (!SessionStore::Instance().Reconnect(rreq, &rrsp) || !rrsp.ok())
        return Fail("reconnect");
    const std::string token3 = rrsp.token();
    const uint64_t gen3 = rrsp.generation();
    if (token3 == token2 || gen3 <= gen2)
        return Fail("reconnect did not rotate");

    // 迟到的旧 MarkDisconnected
    if (SessionStore::Instance().MarkDisconnected(pid, token2, gen2))
        return Fail("stale mark should be ignored");
    if (!SessionStore::Instance().ValidateToken(pid, token3, &err))
        return Fail("new token after stale mark");

    // AcquireSession 返回 GameLogic 路由字段
    SessionStore::Instance().SetLogicInstanceIds({"gl-0", "gl-1"});
    AcquireSessionInput ain;
    ain.account_id = pid;
    ain.player_id = pid;
    ain.device_id = "dev-route";
    ain.server_id = 1;
    ain.kick_other_device = true;
    ain.gateway_instance_id = "gw-8083";
    AcquireSessionResult aout;
    if (!SessionStore::Instance().AcquireSession(ain, &aout) || !aout.ok)
        return Fail("AcquireSession");
    if (aout.session_id.empty() || aout.fence_token.empty())
        return Fail("AcquireSession session/fence");
    if (aout.gamelogic_instance_id != "gl-0" && aout.gamelogic_instance_id != "gl-1")
        return Fail("AcquireSession gamelogic_instance_id");
    if (aout.route_version == 0)
        return Fail("AcquireSession route_version");

    // MarkDisconnected ≠ Logout：仍可在宽限内 Reconnect（换 Gateway）
    if (!SessionStore::Instance().MarkDisconnected(pid, aout.fence_token, aout.generation))
        return Fail("MarkDisconnected after Acquire");
    game::ReconnectReq r2;
    r2.set_player_id(pid);
    r2.set_session_id(aout.session_id);
    r2.set_reconnect_ticket(aout.fence_token);
    game::ReconnectRsp rr2;
    if (!SessionStore::Instance().Reconnect(r2, &rr2) || !rr2.ok())
        return Fail("Reconnect after MarkDisconnected (cross-GW allowed)");

    std::printf("OK session_store_test login/replace/disconnect/reconnect/route\n");
    std::printf("PASS session_store_test\n");
    return 0;
}
