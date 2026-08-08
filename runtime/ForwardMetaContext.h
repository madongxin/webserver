#pragma once

#include <cstdint>
#include <string>

/** 与 fwd.ForwardMeta 对齐的轻量上下文（避免非 brpc 构建依赖 forward.pb） */
struct ForwardRouteMeta {
    uint64_t player_id = 0;
    uint64_t connection_id = 0;
    uint64_t generation = 0;
    uint64_t map_instance_id = 0;
    uint64_t owner_epoch = 0;
    uint64_t route_version = 0;
    std::string gamelogic_instance_id;
    std::string session_id;
    std::string fence_token;
};

/** PlayerSerialQueue worker 线程内的当前 Forward 路由元数据 */
class ForwardMetaContext {
public:
    static void Set(const ForwardRouteMeta &meta);
    static void Clear();
    static const ForwardRouteMeta *Get();
};
