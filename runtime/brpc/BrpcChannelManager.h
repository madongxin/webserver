#pragma once

#include "RpcChannelSnapshot.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace brpc {
class Channel;
}

/**
 * 长生命周期 brpc::Channel：不可变 LogicChannelPoolSnapshot 热更新。
 * RPC 调用方须在在途期间持有 shared_ptr，避免 ApplySnapshot 移除后 UAF。
 * Channel::Init 在锁外完成。
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
    uint64_t snapshot_version() const;

    std::shared_ptr<brpc::Channel> SharedChannelForPlayer(uint64_t player_id);
    std::shared_ptr<brpc::Channel> SharedChannelForInstance(const std::string &gamelogic_instance_id);

    /** @deprecated 优先 SharedChannel*；裸指针仅在锁外立即使用且无异步回调时安全 */
    brpc::Channel *ChannelForPlayer(uint64_t player_id);
    brpc::Channel *ChannelForInstance(const std::string &gamelogic_instance_id);

private:
    BrpcChannelManager() = default;
    ~BrpcChannelManager();

    static std::shared_ptr<brpc::Channel> MakeChannel(const std::string &addr, int timeout_ms);
    std::shared_ptr<const LogicChannelPoolSnapshot> Current() const;
    void Publish(std::shared_ptr<LogicChannelPoolSnapshot> next);

    std::shared_ptr<const LogicChannelPoolSnapshot> snap_{
        std::make_shared<LogicChannelPoolSnapshot>()};
    std::atomic<uint64_t> version_{0};
    std::atomic<uint64_t> empty_snapshot_ignored_{0};
    int timeout_ms_ = 3000;
};
