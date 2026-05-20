#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>

class EventLoop;

struct PendingPlayerItem {
    uint64_t player_id = 0;
    uint64_t item_id = 0;
    uint32_t count = 0;
    int64_t expire_time_sec = 0;
    std::string extra_data;
};

// 在线玩家道具变更先入队，每 5 分钟批量落库 player_item
class PlayerItemPersistQueue {
public:
    static PlayerItemPersistQueue &Instance();

    void StartPeriodic(EventLoop *loop, double interval_sec = 300.0);
    void Enqueue(const PendingPlayerItem &item);
    void MarkOnline(uint64_t player_id);
    void MarkOffline(uint64_t player_id);
    size_t QueueSize() const;

private:
    PlayerItemPersistQueue() = default;
    void OnFlushTick();
    void FlushOnlinePlayers();
    void FlushPlayer(uint64_t player_id);

    mutable std::mutex mu_;
    std::deque<PendingPlayerItem> queue_;
    std::unordered_set<uint64_t> online_local_;
};
