/**
 * @file message_route_test.cpp
 * @brief 阶段 6：Gateway 消息类型路由分类
 */

#include "MessageRoute.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>

namespace {

int g_fail = 0;

#define EXPECT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_fail;                                                                              \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    {
        game::GameRequest req;
        req.mutable_mail_claim()->set_player_id(1);
        EXPECT_TRUE(gameproto::IsWorldBoundRequest(req));
        EXPECT_TRUE(!gameproto::IsLogicBoundRequest(req));
    }
    {
        game::GameRequest req;
        req.mutable_chat_send()->set_player_id(1);
        EXPECT_TRUE(gameproto::IsWorldBoundRequest(req));
    }
    {
        game::GameRequest req;
        req.mutable_friend_list()->set_player_id(1);
        EXPECT_TRUE(gameproto::IsWorldBoundRequest(req));
    }
    {
        game::GameRequest req;
        req.mutable_get_player_brief()->set_player_id(1);
        EXPECT_TRUE(gameproto::IsWorldBoundRequest(req));
        EXPECT_TRUE(!gameproto::IsMailBoundRequest(req));
    }
    {
        game::GameRequest req;
        req.mutable_query_online_state()->set_player_id(1);
        EXPECT_TRUE(gameproto::IsWorldBoundRequest(req));
    }
    {
        game::GameRequest req;
        req.mutable_enter_map()->set_player_id(1);
        req.mutable_enter_map()->set_map_template_id(9);
        EXPECT_TRUE(gameproto::IsLogicBoundRequest(req));
        EXPECT_TRUE(!gameproto::IsWorldBoundRequest(req));
    }
    {
        game::GameRequest req;
        req.mutable_player_mail_send()->set_sender_player_id(1);
        EXPECT_TRUE(gameproto::IsMailBoundRequest(req));
        EXPECT_TRUE(gameproto::IsWorldBoundRequest(req));
    }
    {
        game::GameRequest req;
        req.mutable_chat_send()->set_player_id(1);
        EXPECT_TRUE(!gameproto::IsMailBoundRequest(req));
        EXPECT_TRUE(gameproto::IsWorldBoundRequest(req));
    }
    {
        game::GameRequest req;
        req.mutable_login()->set_player_id(1);
        EXPECT_TRUE(gameproto::IsLogicBoundRequest(req));
    }

    if (g_fail) {
        std::fprintf(stderr, "FAILED %d\n", g_fail);
        return 1;
    }
    std::printf("OK message_route_test\n");
    return 0;
}
