/**
 * Remediation 阶段 1：连接索引条件删除、AuthFlow 代次、GatewayIdentity
 */
#include "GatewayAuthFlow.h"
#include "GatewayConnRegistry.h"
#include "GatewayIdentity.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int g_fail = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::cerr << "FAIL: " << m << "\n";
        ++g_fail;
    } else {
        std::cout << "ok: " << m << "\n";
    }
}

void TestConditionalForgetAfterTakeover() {
    auto &reg = GatewayConnRegistry::Instance();
    reg.Forget(1);
    reg.Forget(2);

    GatewayConnRegistry::Bind oldb;
    oldb.player_id = 100;
    oldb.session_id = "sess-1";
    oldb.token = "fence-old";
    oldb.generation = 1;
    oldb.gateway_instance_id = "gw-0";
    reg.Remember(1, oldb);

    GatewayConnRegistry::Bind newb = oldb;
    newb.token = "fence-new";
    newb.generation = 2;
    newb.gateway_instance_id = "gw-1";
    reg.Remember(2, newb);  // 同 player/session 接管

    GatewayConnRegistry::Bind found;
    Expect(reg.FindByPlayer(100, &found) && found.connection_id == 2, "player index -> new conn");
    Expect(reg.FindBySession("sess-1", &found) && found.connection_id == 2,
           "session index -> new conn");

    reg.Forget(1);  // 旧连接关闭：不得拆掉新索引
    Expect(reg.FindByPlayer(100, &found) && found.connection_id == 2,
           "after old Forget player index intact");
    Expect(reg.FindBySession("sess-1", &found) && found.connection_id == 2,
           "after old Forget session index intact");
    Expect(!reg.FindByConnection(1, &found), "old by_conn removed");

    reg.Forget(2);
}

void TestRememberDoesNotClobberForeignIndex() {
    auto &reg = GatewayConnRegistry::Instance();
    reg.Forget(10);
    reg.Forget(11);

    GatewayConnRegistry::Bind a;
    a.player_id = 1;
    a.session_id = "s-a";
    a.gateway_instance_id = "gw-0";
    reg.Remember(10, a);

    GatewayConnRegistry::Bind b;
    b.player_id = 1;
    b.session_id = "s-a";
    b.gateway_instance_id = "gw-1";
    reg.Remember(11, b);

    // 连接 10 再 Remember 其它身份时，不得擦掉已被 11 接管的 s-a
    GatewayConnRegistry::Bind a2;
    a2.player_id = 2;
    a2.session_id = "s-other";
    a2.gateway_instance_id = "gw-0";
    reg.Remember(10, a2);

    GatewayConnRegistry::Bind found;
    Expect(reg.FindBySession("s-a", &found) && found.connection_id == 11,
           "Remember on old conn does not erase taken session");
    reg.Forget(10);
    reg.Forget(11);
}

void TestAuthFlowGeneration() {
    auto &flow = GatewayAuthFlow::Instance();
    const uint64_t c = 77;
    flow.OnConnected(c);
    uint64_t g1 = 0, g2 = 0;
    Expect(flow.TryBegin(c, &g1), "first TryBegin");
    Expect(!flow.TryBegin(c, &g2), "second TryBegin rejected while in-flight");
    Expect(flow.AcceptCallback(c, g1), "accept matching gen");
    flow.OnDisconnected(c);
    Expect(!flow.AcceptCallback(c, g1), "reject after disconnect");
    flow.End(c, g1);

    flow.OnConnected(c);
    Expect(flow.TryBegin(c, &g1), "begin after reconnect slot");
    flow.End(c, g1);
    flow.OnDisconnected(c);
}

void TestGatewayIdentityEnv() {
    setenv("GAMEMESH_INSTANCE_ID", "gw-race-test", 1);
    unsetenv("GAMEMESH_FORMAL");
    GatewayIdentity::Instance().Set("");  // 清空后重新 Resolve
    std::string err;
    Expect(GatewayIdentity::Instance().Resolve(&err), "resolve from env");
    Expect(GatewayIdentity::Instance().id() == "gw-race-test", "id matches env");
    Expect(GatewayIdentity::Instance().id().find("0.0.0.0") == std::string::npos,
           "id not listen-derived");
    // GameTCP 与 Push 共用同一 Resolve 结果（启动路径契约）
    Expect(GatewayIdentity::Instance().id() == GatewayInstanceId(), "GatewayInstanceId alias");
}

}  // namespace

int main() {
    TestConditionalForgetAfterTakeover();
    TestRememberDoesNotClobberForeignIndex();
    TestAuthFlowGeneration();
    TestGatewayIdentityEnv();
    if (g_fail) {
        std::cerr << "gateway_conn_race_test FAIL count=" << g_fail << "\n";
        return 1;
    }
    std::cout << "gateway_conn_race_test PASS\n";
    return 0;
}
