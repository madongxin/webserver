#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/**
 * 按 player_id 分片的串行队列：同一 player 严格有序，不同 player 可并行。
 * 阶段 1 最小实现（非完整 Actor）。
 */
class PlayerSerialQueue {
public:
    static PlayerSerialQueue &Instance();

    /** shard_count<=0 时按硬件并发估算 */
    void Start(int shard_count = 0);
    void Stop();

    bool started() const { return started_; }

    void Post(uint64_t player_id, std::function<void()> task);

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
};
