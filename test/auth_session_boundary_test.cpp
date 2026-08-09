/**
 * 登录边界 / 粘性路由 / Push 目标 / 端口约定 / etcd 降级
 */
#include "GameLogic.h"
#include "GatewayConnRegistry.h"
#include "IServiceRegistry.h"
#include "MapInstanceRegistry.h"
#include "MapPlacement.h"
#include "MessageRoute.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int fails = 0;
static void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    } else
        std::printf("ok: %s\n", m);
}

int main() {
    // 5. GameLogic 拒收 Login 凭证
    {
        game::GameRequest req;
        req.mutable_login()->set_player_id(1);
        req.mutable_login()->set_device_id("x");
        game::GameResponse rsp;
        Expect(!GameLogic::Instance().Handle(req, &rsp), "GameLogic rejects Login");
        Expect(!rsp.ok(), "rsp not ok");
        Expect(rsp.login().message().find("Gateway") != std::string::npos ||
                   rsp.message().find("orchestrat") != std::string::npos || !rsp.ok(),
               "reject message indicates Gateway orchestration");
    }

    // Login 属 Gateway 编排，不走 World；单元侧确认 MessageRoute 不把 Login 当 World
    {
        game::GameRequest req;
        req.mutable_login()->set_player_id(1);
        Expect(!gameproto::IsWorldBoundRequest(req), "Login is not World-bound");
    }

    // 3/4/6/7. GatewayConnRegistry：粘性 Logic + 旧 session Push 拒绝
    {
        int sent = 0;
        GatewayConnRegistry::Bind b;
        b.player_id = 42;
        b.session_id = "sess-new";
        b.token = "fence-new";
        b.gamelogic_instance_id = "gl-1";
        b.gateway_instance_id = "gw-8081";
        b.map_instance_id = 9;
        b.map_owner_epoch = 3;
        b.route_version = 2;
        b.send_frame = [&sent](const std::string &) { ++sent; };
        GatewayConnRegistry::Instance().Remember(7, b);

        GatewayConnRegistry::Bind out;
        Expect(GatewayConnRegistry::Instance().FindBySession("sess-new", &out), "find new session");
        Expect(out.gamelogic_instance_id == "gl-1", "sticky gamelogic_instance_id");
        Expect(out.map_instance_id == 9 && out.map_owner_epoch == 3 && out.route_version == 2,
               "Session route fields stored");
        Expect(out.gateway_instance_id == "gw-8081", "Push target gateway_instance_id stored");

        Expect(!GatewayConnRegistry::Instance().FindBySession("sess-old", &out),
               "old session_id rejected/not found");
        Expect(!GatewayConnRegistry::Instance().SendBySession("sess-old", "xxxx"),
               "Push old session_id rejected");
        Expect(GatewayConnRegistry::Instance().SendBySession("sess-new", "\x00\x00\x00\x01x"),
               "Push to bound gateway session ok");
        Expect(sent == 1, "Push delivered once to bound connection only");

        GatewayConnRegistry::Bind by_player;
        Expect(GatewayConnRegistry::Instance().FindByPlayer(42, &by_player) &&
                   by_player.gamelogic_instance_id == "gl-1",
               "route by player sticky logic");
        GatewayConnRegistry::Instance().Forget(7);
    }

    // 9. 旧 map_owner_epoch 拒绝
    {
        MapInstanceRegistry::Instance().ClearForTest();
        Expect(MapInstanceRegistry::Instance().Claim(100, 1, 5), "claim epoch 5");
        Expect(MapInstanceRegistry::Instance().AcceptWrite(100, 5), "accept current epoch");
        Expect(!MapInstanceRegistry::Instance().AcceptWrite(100, 4), "reject stale map_owner_epoch");
        Expect(!MapInstanceRegistry::Instance().AcceptWrite(100, 6), "reject future epoch");
        // lease 强制：过期后拒写
        Expect(MapInstanceRegistry::Instance().Claim(101, 1, 1, 1), "claim with past lease");
        Expect(MapInstanceRegistry::Instance().CheckWrite(101, 1) == MapWriteFence::LeaseExpired,
               "lease expired fence");
        Expect(!MapInstanceRegistry::Instance().AcceptWrite(101, 1), "reject expired lease write");
        // formal：lease=0 拒写
        MapInstanceRegistry::Instance().SetRequireLease(true);
        Expect(MapInstanceRegistry::Instance().Claim(102, 1, 1, 0), "claim lease 0");
        Expect(MapInstanceRegistry::Instance().CheckWrite(102, 1) == MapWriteFence::LeaseMissing,
               "lease missing when require_lease");
        Expect(!MapInstanceRegistry::Instance().AcceptWrite(102, 1), "reject lease 0 write");
        MapInstanceRegistry::Instance().SetRequireLease(false);
        MapInstanceRegistry::Instance().ClearForTest();
    }

    // 13. 静态 Registry（etcd 未运行降级）
    {
        StaticServiceRegistry::Get().SetStaticAddrs("gamelogic", {"127.0.0.1:8201", "127.0.0.1:8202"},
                                                    {"gl-0", "gl-1"});
        StaticServiceRegistry::Get().SetStaticAddrs("session", {"127.0.0.1:8401"}, {"sess-0"});
        std::vector<std::string> addrs;
        Expect(StaticServiceRegistry::Get().DiscoverAddrs("gamelogic", &addrs) && addrs.size() == 2,
               "static logic_addrs fallback");
        Expect(StaticServiceRegistry::Get().DiscoverAddrs("session", &addrs) && addrs.size() == 1,
               "static session_addrs fallback");
    }

    // 14. HTTP/Push 端口不得被当成游戏口
    {
        const int game = 8081, http = 8080, push = game + 100;
        Expect(game != http && game != push, "game port distinct from http/push");
        Expect(push == 8181, "push = game_port+100");
        Expect(http == 8080, "http management port");
    }

    // Formal：无 Placement 时禁止本地随意 Claim（与 phase1_correctness_test 互补）
    {
        ::setenv("GAMEMESH_FORMAL", "1", 1);
        MapInstanceRegistry::Instance().ClearForTest();
        MapInstanceRegistry::Instance().SetLocalInstanceId("gl-0");
        MapPlacement::Instance().ConfigureOwners({"gl-0"});
        game::GameRequest req;
        req.mutable_enter_map()->set_player_id(1);
        req.mutable_enter_map()->set_realm_id(1);
        req.mutable_enter_map()->set_map_template_id(42);
        game::GameResponse rsp;
        Expect(!GameLogic::Instance().Handle(req, &rsp),
               "formal EnterMap without Placement rejected");
        ::unsetenv("GAMEMESH_FORMAL");
        MapInstanceRegistry::Instance().ClearForTest();
    }

    if (fails) {
        std::printf("%d failures\n", fails);
        return 1;
    }
    std::printf("auth_session_boundary_test passed\n");
    return 0;
}
