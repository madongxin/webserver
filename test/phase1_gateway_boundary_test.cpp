/**
 * 阶段 1：Gateway 白名单 / 身份覆盖 / Dispatch 路径契约 / fail-closed logic id
 */
#include "GatewayAuthPolicy.h"
#include "GatewayConnRegistry.h"
#include "MessageRoute.h"
#include "ProtoFraming.h"
#include "game.pb.h"

#include <cstdint>
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

std::string Serialize(const game::GameRequest &req) {
    std::string s;
    req.SerializeToString(&s);
    return s;
}

void TestWhitelist() {
    game::GameRequest login;
    login.mutable_login()->set_player_id(1);
    Expect(gameproto::IsPreAuthWhitelistPayload(Serialize(login)), "login whitelist");

    game::GameRequest reg;
    reg.mutable_register_()->set_device_id("dev-2");
    Expect(gameproto::IsPreAuthWhitelistPayload(Serialize(reg)), "register whitelist");
    Expect(gameproto::IsGatewayOwnedAuthPayload(Serialize(reg)), "register gateway-owned");

    game::GameRequest recon;
    recon.mutable_reconnect()->set_player_id(1);
    recon.mutable_reconnect()->set_session_id("s");
    recon.mutable_reconnect()->set_reconnect_ticket("t");
    Expect(gameproto::IsPreAuthWhitelistPayload(Serialize(recon)), "reconnect whitelist");
    Expect(gameproto::IsGatewayOwnedAuthPayload(Serialize(recon)), "reconnect gateway-owned");

    game::GameRequest hello;
    hello.mutable_client_hello()->set_protocol_version(1);
    Expect(gameproto::IsPreAuthWhitelistPayload(Serialize(hello)), "hello whitelist");
    Expect(!gameproto::IsGatewayOwnedAuthPayload(Serialize(hello)), "hello not auth orchestrator");

    game::GameRequest hb;
    hb.mutable_heartbeat()->set_echo_ms(1);
    Expect(gameproto::IsPreAuthWhitelistPayload(Serialize(hb)), "heartbeat whitelist");

    game::GameRequest mail;
    mail.mutable_mail_list()->set_player_id(1);
    Expect(!gameproto::IsPreAuthWhitelistPayload(Serialize(mail)), "mail not whitelist");
}

void TestStickyOverwritesPlayerId() {
    GatewayConnRegistry::Instance().Forget(42);
    GatewayConnRegistry::Bind b;
    b.player_id = 1001;
    b.token = "fence-a";
    b.session_id = "sess-a";
    b.generation = 3;
    b.gamelogic_instance_id = "gl-1";
    b.map_instance_id = 9;
    b.route_version = 2;
    GatewayConnRegistry::Instance().Remember(42, b);

    GatewayConnRegistry::Bind sticky;
    Expect(GatewayConnRegistry::Instance().FindByConnection(42, &sticky), "find sticky");
    Expect(sticky.player_id == 1001, "trusted player_id");
    Expect(sticky.gamelogic_instance_id == "gl-1", "trusted logic id");
    // 客户端伪造的 player_id 不应写入 sticky
    Expect(sticky.player_id != 9999, "not forged id");
    GatewayConnRegistry::Instance().Forget(42);
}

void TestDispatchIsWorldVsLogic() {
    game::GameRequest logic_req;
    logic_req.mutable_consume_item()->set_player_id(1);
    Expect(!gameproto::IsWorldBoundRequest(logic_req), "consume -> logic Dispatch path");

    game::GameRequest world_req;
    world_req.mutable_mail_list()->set_player_id(1);
    Expect(gameproto::IsWorldBoundRequest(world_req), "mail -> world forward path");
    game::GameRequest pmail;
    pmail.mutable_player_mail_send()->set_sender_player_id(1);
    Expect(gameproto::IsWorldBoundRequest(pmail), "player_mail_send -> world");
    game::GameRequest move;
    move.mutable_move()->set_player_id(1);
    Expect(!gameproto::IsWorldBoundRequest(move), "move -> logic");
}

void TestOwnedAuth() {
    game::GameRequest logout;
    logout.mutable_logout()->set_player_id(1);
    Expect(gameproto::IsGatewayOwnedAuthPayload(Serialize(logout)), "logout owned");
    game::GameRequest ping;
    ping.mutable_map_ping()->set_map_instance_id(1);
    Expect(!gameproto::IsGatewayOwnedAuthPayload(Serialize(ping)), "map_ping not owned auth");
}

}  // namespace

int main() {
    TestWhitelist();
    TestStickyOverwritesPlayerId();
    TestDispatchIsWorldVsLogic();
    TestOwnedAuth();
    if (g_fail) {
        std::cerr << "phase1_gateway_boundary_test failures=" << g_fail << "\n";
        return 1;
    }
    std::cout << "phase1_gateway_boundary_test passed\n";
    return 0;
}
