#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace brpc {
class Channel;
}

/**
 * 长生命周期 brpc::Channel：启动期 Init；运行期 ApplySnapshot 热更新。
 * - ChannelForPlayer：无地图绑定时 player_id % N
 * - ChannelForInstance：按 gamelogic_instance_id
 * - 空发现结果不得清空健康 Channel
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
    /**
     * 按 instance_id 合并：同地址复用；新 id 建 Channel；消失的 id 移除。
     * addrs 为空时保留现有 Channel 并返回 false（降级保护）。
     */
    bool ApplySnapshot(const std::vector<std::string> &addrs,
                       const std::vector<std::string> &instance_ids);
    void Shutdown();

    bool ready() const;
    size_t size() const;
    std::vector<std::string> instance_ids() const;
    /** 降级：最近一次空快照被忽略 */
    uint64_t empty_snapshot_ignored() const;

    brpc::Channel *ChannelForPlayer(uint64_t player_id);
    brpc::Channel *ChannelForInstance(const std::string &gamelogic_instance_id);

private:
    BrpcChannelManager() = default;
    ~BrpcChannelManager();

    bool InitUnlocked(const std::vector<std::string> &addrs,
                      const std::vector<std::string> &instance_ids, int timeout_ms);
    static std::unique_ptr<brpc::Channel> MakeChannel(const std::string &addr, int timeout_ms);

    mutable std::mutex mu_;
    std::vector<std::unique_ptr<brpc::Channel>> channels_;
    std::vector<std::string> instance_ids_;
    std::vector<std::string> addrs_;
    std::unordered_map<std::string, size_t> id_to_index_;
    int timeout_ms_ = 3000;
    uint64_t empty_snapshot_ignored_ = 0;
};
