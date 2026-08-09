#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/**
 * 按 player_id 分片的串行队列：同一 player 严格有序，不同 player 可并行。
 * 有界：分片深度 + 全局待处理上限；满则 TryPost 失败（调用方返回过载）。
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
        bool stop = false;
        int inflight = 0;
        std::thread worker;
    };

    void WorkerLoop(Shard *shard);
    static size_t ShardIndex(uint64_t player_id, size_t n);

    std::mutex life_mu_;
    bool started_ = false;
    std::vector<std::unique_ptr<Shard>> shards_;
    size_t max_per_shard_ = 256;
    size_t max_global_ = 4096;
    std::atomic<size_t> pending_global_{0};
};
