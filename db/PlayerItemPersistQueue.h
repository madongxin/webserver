#pragma once

/**
 * @file PlayerItemPersistQueue.h
 * @brief 在线玩家道具发放异步落库队列
 *
 * 设计动机：
 *   - grant_item 时只改内存背包（GameLogic::inventory_），避免每条 RPC 都打 MySQL。
 *   - 待写记录进入 queue_，由定时任务批量 INSERT。
 *
 * 刷盘策略：
 *   1. 每 interval_sec（默认 300s = 5 分钟）FlushOnlinePlayers：
 *      仅处理「当前在线」玩家的队列项（Redis game:session:{uid} 存在；
 *      无 Redis 时用 online_local_）。
 *   2. 玩家 logout 时 FlushPlayer：该玩家队列立即落库。
 *   3. 离线玩家队列项保留，待其再次上线后由定时任务写入。
 *
 * 线程安全：queue_ / online_local_ 由 mu_ 保护；刷盘在独立线程执行（见 OnFlushTick）。
 */

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>

class EventLoop;

/** 待持久化的一条道具发放（与 GrantItemReq 字段对应） */
struct PendingPlayerItem {
    uint64_t player_id = 0;
    uint64_t item_id = 0;
    uint32_t count = 0;
    int64_t expire_time_sec = 0;
    std::string extra_data;
};

class PlayerItemPersistQueue {
public:
    static PlayerItemPersistQueue &Instance();

    /** 注册到主 EventLoop 定时器（与 MetricsDbWriter 类似） */
    void StartPeriodic(EventLoop *loop, double interval_sec = 300.0);

    /** grant_item 成功后调用：入队 + 标记本地在线 */
    void Enqueue(const PendingPlayerItem &item);

    /** 登录成功等：加入本地在线集合（Redis 仍为准） */
    void MarkOnline(uint64_t player_id);

    /** 登出：先刷该玩家队列，再从本地在线集合移除 */
    void MarkOffline(uint64_t player_id);

    size_t QueueSize() const;

private:
    PlayerItemPersistQueue() = default;
    void OnFlushTick();
    void FlushOnlinePlayers();
    void FlushPlayer(uint64_t player_id);

    mutable std::mutex mu_;
    std::deque<PendingPlayerItem> queue_;
    /** Redis 不可用时的在线玩家兜底集合 */
    std::unordered_set<uint64_t> online_local_;
};
