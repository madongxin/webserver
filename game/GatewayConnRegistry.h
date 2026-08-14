#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

/** Gateway 连接表：Push 按 session_id 查找；转发按粘性 gamelogic_instance_id。 */
class GatewayConnRegistry {
public:
    struct Bind {
        uint64_t player_id = 0;
        std::string token;  // fence_token
        std::string session_id;
        uint64_t generation = 0;
        uint64_t connection_id = 0;
        std::string gamelogic_instance_id;
        uint64_t map_instance_id = 0;
        uint64_t map_owner_epoch = 0;
        uint64_t route_version = 0;
        std::string gateway_instance_id;
        std::function<void(const std::string &frame)> send_frame;
        std::function<void()> close_conn;
    };

    static GatewayConnRegistry &Instance();

    void Remember(uint64_t connection_id, Bind bind);
    void Forget(uint64_t connection_id);
    /**
     * 权威路由 cutover：仅当 connection 仍绑定且新 route_version >= 旧值时更新。
     * 返回 false 表示连接已换人或版本过旧。
     */
    bool ApplyRoute(uint64_t connection_id, const std::string &gamelogic_instance_id,
                    uint64_t map_instance_id, uint64_t map_owner_epoch, uint64_t route_version);
    bool FindBySession(const std::string &session_id, Bind *out);
    bool FindByConnection(uint64_t connection_id, Bind *out);
    bool FindByPlayer(uint64_t player_id, Bind *out);
    bool SendBySession(const std::string &session_id, const std::string &frame);
    /** 仅关闭匹配 player_id+session_id+generation 的旧连接；generation=0 时忽略 generation */
    bool CloseIfMatch(uint64_t player_id, const std::string &session_id, uint64_t generation);

private:
    GatewayConnRegistry() = default;
    std::mutex mu_;
    std::unordered_map<uint64_t, Bind> by_conn_;
    std::unordered_map<std::string, uint64_t> session_to_conn_;
    std::unordered_map<uint64_t, uint64_t> player_to_conn_;
};
