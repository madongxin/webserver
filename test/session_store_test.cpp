/**
 * Session 状态机 / 顶号 fence / 重连 / 并发 Acquire（需 Redis）
 */
#include "SessionStore.h"
#include "game.pb.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int Fail(const char *msg) {
    std::printf("FAIL %s\n", msg);
    return 1;
}

}  // namespace

int main() {
    if (!SessionStore::Instance().InitFromConfig()) {
        std::printf("FAIL session_store_test (Redis unavailable)\n");
        return 1;
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

    {
        AcquireSessionInput ain2 = ain;
        ain2.device_id = "dev-route-2";
        ain2.gateway_instance_id = "gw-8081";
        AcquireSessionResult aout2;
        if (!SessionStore::Instance().AcquireSession(ain2, &aout2) || !aout2.ok)
            return Fail("second AcquireSession");
        if (!aout2.kicked_previous)
            return Fail("kicked_previous");
        if (aout2.previous_session_id != aout.session_id)
            return Fail("previous_session_id");
        if (aout2.previous_generation != aout.generation)
            return Fail("previous_generation");
        if (aout2.previous_gateway_instance_id != "gw-8083")
            return Fail("previous_gateway");
        if (aout2.previous_fence_token != aout.fence_token)
            return Fail("previous_fence");
        std::string rerr;
        if (!SessionStore::Instance().RestorePreviousSession(pid, aout2.fence_token, aout2, "",
                                                             &rerr))
            return Fail(("restore previous " + rerr).c_str());
        if (!SessionStore::Instance().ValidateToken(pid, aout.fence_token, &err))
            return Fail("old fence valid after restore");
    }

    // MarkDisconnected ≠ Logout：仍可在宽限内 Reconnect（换 Gateway）
    if (!SessionStore::Instance().MarkDisconnected(pid, aout.fence_token, aout.generation))
        return Fail("MarkDisconnected after Acquire");
    game::ReconnectReq r2;
    r2.set_player_id(pid);
    r2.set_session_id(aout.session_id);
    r2.set_reconnect_ticket(aout.fence_token);
    game::ReconnectRsp rr2;
    SessionRecord route;
    if (!SessionStore::Instance().Reconnect(r2, &rr2, &route) || !rr2.ok())
        return Fail("Reconnect after MarkDisconnected (cross-GW allowed)");
    if (route.gamelogic_instance_id.empty() || route.route_version == 0)
        return Fail("Reconnect route fields");

    // 并发 Acquire：Lua 原子递增 generation，最终只保留一个权威 fence
    const uint64_t pid2 = 900002;
    {
        game::LogoutReq lo2;
        lo2.set_player_id(pid2);
        game::LogoutRsp lorsp2;
        SessionStore::Instance().Logout(lo2, &lorsp2);
    }
    constexpr int kThreads = 8;
    std::atomic<int> ok_count{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            AcquireSessionInput cin;
            cin.player_id = pid2;
            cin.device_id = "dev-c-" + std::to_string(i);
            cin.server_id = 1;
            cin.kick_other_device = true;
            cin.gateway_instance_id = "gw-c";
            AcquireSessionResult cout;
            if (SessionStore::Instance().AcquireSession(cin, &cout) && cout.ok)
                ok_count.fetch_add(1);
        });
    }
    for (auto &t : threads)
        t.join();
    if (ok_count.load() != kThreads)
        return Fail("concurrent acquire not all ok");
    if (!SessionStore::Instance().IsPlayerOnline(pid2))
        return Fail("concurrent acquire not online");
    AcquireSessionInput cin2;
    cin2.player_id = pid2;
    cin2.device_id = "dev-final";
    cin2.kick_other_device = true;
    AcquireSessionResult cout2;
    if (!SessionStore::Instance().AcquireSession(cin2, &cout2) || !cout2.ok)
        return Fail("post-concurrent acquire");
    if (cout2.generation < static_cast<uint64_t>(kThreads + 1))
        return Fail("generation not monotonic under concurrent acquire");

    // operation_id 幂等：同键重试返回同一 session/fence，不产生第二会话
    const uint64_t pid3 = 900003;
    {
        game::LogoutReq lo3;
        lo3.set_player_id(pid3);
        game::LogoutRsp lorsp3;
        SessionStore::Instance().Logout(lo3, &lorsp3);
    }
    AcquireSessionInput idem;
    idem.player_id = pid3;
    idem.device_id = "dev-idem";
    idem.server_id = 1;
    idem.kick_other_device = true;
    idem.operation_id = "test-acq-idem-900003";
    AcquireSessionResult i1;
    if (!SessionStore::Instance().AcquireSession(idem, &i1) || !i1.ok)
        return Fail("idem acquire1");
    AcquireSessionResult i2;
    if (!SessionStore::Instance().AcquireSession(idem, &i2) || !i2.ok)
        return Fail("idem acquire2");
    if (i1.session_id != i2.session_id || i1.fence_token != i2.fence_token ||
        i1.generation != i2.generation)
        return Fail("idem acquire not same session/fence");
    SessionOpStatus st = SessionOpStatus::NotFound;
    std::string kind;
    AcquireSessionResult ig;
    if (!SessionStore::Instance().GetSessionOperation(idem.operation_id, &st, &kind, &ig) ||
        st != SessionOpStatus::Done || kind != "acquire" || ig.session_id != i1.session_id)
        return Fail("GetSessionOperation acquire");

    // 零健康 GameLogic：AcquireSession fail-closed，不创建半完成 Session
    {
        const uint64_t pid4 = 900004;
        {
            game::LogoutReq lo4;
            lo4.set_player_id(pid4);
            game::LogoutRsp lorsp4;
            SessionStore::Instance().Logout(lo4, &lorsp4);
        }
        SessionStore::Instance().SetLogicInstanceIds({});
        AcquireSessionInput empty_in;
        empty_in.player_id = pid4;
        empty_in.device_id = "dev-empty";
        empty_in.server_id = 1;
        empty_in.kick_other_device = true;
        empty_in.preferred_gamelogic_instance_id = "gl-0";
        AcquireSessionResult empty_out;
        if (SessionStore::Instance().AcquireSession(empty_in, &empty_out) || empty_out.ok)
            return Fail("empty owners acquire should fail");
        if (empty_out.error_code != "NO_HEALTHY_GAMELOGIC")
            return Fail("empty owners error_code");
        if (SessionStore::Instance().IsPlayerOnline(pid4))
            return Fail("empty owners created session");
        SessionStore::Instance().SetLogicInstanceIds({"gl-0", "gl-1"});
        AcquireSessionInput ok_in = empty_in;
        ok_in.preferred_gamelogic_instance_id = "gl-dead";
        AcquireSessionResult ok_out;
        if (!SessionStore::Instance().AcquireSession(ok_in, &ok_out) || !ok_out.ok)
            return Fail("restore owners acquire");
        if (ok_out.gamelogic_instance_id == "gl-dead")
            return Fail("preferred dead owner used");
        if (ok_out.gamelogic_instance_id != "gl-0" && ok_out.gamelogic_instance_id != "gl-1")
            return Fail("restore owners logic id");
    }

    {
        const uint64_t kpid = 900088;
        game::LogoutReq klo;
        klo.set_player_id(kpid);
        game::LogoutRsp klr;
        SessionStore::Instance().Logout(klo, &klr);
        AcquireSessionInput kin;
        kin.player_id = kpid;
        kin.device_id = "kick-dev";
        kin.server_id = 1;
        kin.kick_other_device = true;
        kin.gateway_instance_id = "gw-0";
        AcquireSessionResult kout;
        if (!SessionStore::Instance().AcquireSession(kin, &kout) || !kout.ok)
            return Fail("kick setup acquire");
        const std::string old_fence = kout.fence_token;
        const uint64_t old_gen = kout.generation;
        SessionStore::KickResult kr;
        if (!SessionStore::Instance().Kick(kpid, "e2e", &kr) || !kr.ok)
            return Fail("kick cas");
        if (kr.new_generation <= old_gen)
            return Fail("kick generation");
        if (kr.old_token != old_fence)
            return Fail("kick old token");
        game::ValidateSessionReq v;
        v.set_player_id(kpid);
        v.set_token(old_fence);
        game::ValidateSessionRsp vr;
        SessionStore::Instance().Validate(v, &vr);
        if (vr.valid())
            return Fail("old fence still valid after kick");
    }

    std::printf("OK session_store_test login/replace/disconnect/reconnect/route/concurrent/idem\n");
    std::printf("PASS session_store_test\n");
    return 0;
}
