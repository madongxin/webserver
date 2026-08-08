#pragma once

#include <cstdint>
#include <string>

/** 业务侧会话句柄（阶段 3 fence + 阶段 5 地图路由） */
struct SessionHandle {
    uint64_t player_id = 0;
    uint64_t connection_id = 0;
    uint64_t generation = 0;
    std::string session_id;
    std::string fence_token;
    uint64_t map_instance_id = 0;
    uint64_t owner_epoch = 0;
    uint64_t route_version = 0;
    std::string gamelogic_instance_id;
};
