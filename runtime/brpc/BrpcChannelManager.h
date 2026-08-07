#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace brpc {
class Channel;
}

/**
 * 长生命周期 brpc::Channel：启动期 Init。
 * - ChannelForPlayer：无地图绑定时 player_id % N
 * - ChannelForInstance：按 gamelogic_instance_id（阶段 5 Placement）
 */
class BrpcChannelManager {
public:
    static BrpcChannelManager &Instance();

    /**
     * @param addrs 地址列表
     * @param instance_ids 与 addrs 一一对应；空则自动 gl-0..gl-N-1
     */
    bool Init(const std::vector<std::string> &addrs, const std::vector<std::string> &instance_ids,
              int timeout_ms = 3000);
    void Shutdown();

    bool ready() const { return !channels_.empty(); }
    size_t size() const { return channels_.size(); }
    const std::vector<std::string> &instance_ids() const { return instance_ids_; }

    brpc::Channel *ChannelForPlayer(uint64_t player_id);
    brpc::Channel *ChannelForInstance(const std::string &gamelogic_instance_id);

private:
    BrpcChannelManager() = default;
    ~BrpcChannelManager();

    std::vector<std::unique_ptr<brpc::Channel>> channels_;
    std::vector<std::string> instance_ids_;
    std::unordered_map<std::string, size_t> id_to_index_;
    int timeout_ms_ = 3000;
};
