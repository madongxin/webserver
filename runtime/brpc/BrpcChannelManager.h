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
 * 长生命周期 brpc::Channel：shared_ptr 快照热更新。
 * RPC 调用方须在在途期间持有 shared_ptr，避免 ApplySnapshot 移除后 UAF。
 */
class BrpcChannelManager {
public:
    static BrpcChannelManager &Instance();

    bool Init(const std::vector<std::string> &addrs, const std::vector<std::string> &instance_ids,
              int timeout_ms = 3000);
    bool ApplySnapshot(const std::vector<std::string> &addrs,
                       const std::vector<std::string> &instance_ids);
    void Shutdown();

    bool ready() const;
    size_t size() const;
    std::vector<std::string> instance_ids() const;
    uint64_t empty_snapshot_ignored() const;

    std::shared_ptr<brpc::Channel> SharedChannelForPlayer(uint64_t player_id);
    std::shared_ptr<brpc::Channel> SharedChannelForInstance(const std::string &gamelogic_instance_id);

    /** @deprecated 优先 SharedChannel*；裸指针仅在锁外立即使用且无异步回调时安全 */
    brpc::Channel *ChannelForPlayer(uint64_t player_id);
    brpc::Channel *ChannelForInstance(const std::string &gamelogic_instance_id);

private:
    BrpcChannelManager() = default;
    ~BrpcChannelManager();

    bool InitUnlocked(const std::vector<std::string> &addrs,
                      const std::vector<std::string> &instance_ids, int timeout_ms);
    static std::shared_ptr<brpc::Channel> MakeChannel(const std::string &addr, int timeout_ms);

    mutable std::mutex mu_;
    std::vector<std::shared_ptr<brpc::Channel>> channels_;
    std::vector<std::string> instance_ids_;
    std::vector<std::string> addrs_;
    std::unordered_map<std::string, size_t> id_to_index_;
    int timeout_ms_ = 3000;
    uint64_t empty_snapshot_ignored_ = 0;
};
