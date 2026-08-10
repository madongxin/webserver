#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gameproto {

/** Login/Reconnect 编排成功后的路由与可靠 Push 补发载荷（不依赖 brpc 头） */
struct GatewayLoginRoute {
    uint64_t player_id = 0;
    std::string session_id;
    std::string fence_token;
    uint64_t generation = 0;
    std::string gamelogic_instance_id;
    uint64_t map_instance_id = 0;
    uint64_t map_owner_epoch = 0;
    uint64_t route_version = 0;
    bool need_full_snapshot = false;
    uint64_t last_server_seq = 0;
    /** 重连成功且绑定后按序补发的可靠 Push 载荷（已是 GameResponse body） */
    std::vector<std::string> pending_push_payloads;
};

}  // namespace gameproto
