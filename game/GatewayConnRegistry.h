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
        int connection_id = 0;
        std::string gamelogic_instance_id;
        uint64_t map_instance_id = 0;
        uint64_t map_owner_epoch = 0;
        uint64_t route_version = 0;
        std::string gateway_instance_id;
        std::function<void(const std::string &frame)> send_frame;
    };

    static GatewayConnRegistry &Instance();

    void Remember(int connection_id, Bind bind);
    void Forget(int connection_id);
    bool FindBySession(const std::string &session_id, Bind *out);
    bool FindByConnection(int connection_id, Bind *out);
    bool FindByPlayer(uint64_t player_id, Bind *out);
    bool SendBySession(const std::string &session_id, const std::string &frame);

private:
    GatewayConnRegistry() = default;
    std::mutex mu_;
    std::unordered_map<int, Bind> by_conn_;
    std::unordered_map<std::string, int> session_to_conn_;
    std::unordered_map<uint64_t, int> player_to_conn_;
};
