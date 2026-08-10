#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * 按 player_id 分片的串行队列：同一 player 严格有序，不同 player 可并行。
 * 有界：分片深度 + 全局待处理上限；满则 TryPost 失败（调用方返回过载）。
 * 异步下游：MarkAsyncInFlight 后同玩家后续任务进入 deferred，直到 ClearAsyncInFlight。
 */
class PlayerSerialQueue {
public:
    static PlayerSerialQueue &Instance();

    /** shard_count<=0 时按硬件并发估算 */
    void Start(int shard_count = 0);
    void Stop();

    bool started() const { return started_; }

    void SetLimits(size_t max_per_shard, size_t max_global);

    /** 投递成功返回 true；队列满返回 false（不占用内存追加） */
    bool TryPost(uint64_t player_id, std::function<void()> task);

    /** 兼容旧路径：满时丢弃并打日志（Gateway 编排应改用 TryPost） */
    void Post(uint64_t player_id, std::function<void()> task);

    /**
     * 在任务内调用：标记该玩家异步在途，释放 shard worker；
     * 同玩家后续 TryPost 进入 deferred，保证有序。
     */
    void MarkAsyncInFlight(uint64_t player_id);

    /** 异步完成时调用：清 inflight 并将 deferred 拼回主队列 */
    void ClearAsyncInFlight(uint64_t player_id);

    /**
     * 异步完成投递：completion 插到队首，再拼回 deferred，保证先收尾再跑同玩家后续任务。
     */
    bool CompleteAsyncInFlight(uint64_t player_id, std::function<void()> completion);

    bool IsAsyncInFlight(uint64_t player_id) const;

    size_t pending_global() const { return pending_global_.load(std::memory_order_relaxed); }

    /** 测试用：等待所有分片队列变空且无在飞任务 */
    void DrainForTest();

private:
    PlayerSerialQueue() = default;
    ~PlayerSerialQueue();

    struct Shard {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::function<void()>> q;
        std::unordered_map<uint64_t, std::deque<std::function<void()>>> deferred;
        std::unordered_set<uint64_t> async_inflight;
        bool stop = false;
        int inflight = 0;
        std::thread worker;
    };

    void WorkerLoop(Shard *shard);
    static size_t ShardIndex(uint64_t player_id, size_t n);
    Shard *ShardFor(uint64_t player_id);

    std::mutex life_mu_;
    bool started_ = false;
    std::vector<std::unique_ptr<Shard>> shards_;
    size_t max_per_shard_ = 256;
    size_t max_global_ = 4096;
    std::atomic<size_t> pending_global_{0};
};
