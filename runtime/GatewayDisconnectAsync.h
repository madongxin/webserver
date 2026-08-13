#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

/**
 * Gateway 断线本地 Redis 补偿：禁止在 Reactor 线程同步 MarkDisconnected。
 * 有界队列 + 后台 worker；满则丢弃并记指标，依赖 Session TTL 收敛。
 */
class GatewayDisconnectAsync {
public:
    struct Task {
        uint64_t player_id = 0;
        std::string token;
        uint64_t generation = 0;
    };

    static GatewayDisconnectAsync &Instance();

    void Start(size_t max_queue = 4096);
    /** 有界等待排空后停 worker */
    void Stop(std::chrono::milliseconds drain_deadline = std::chrono::milliseconds(2000));

    /** Reactor 线程：仅入队；成功 true，满/未启动 false */
    bool EnqueueMarkDisconnected(uint64_t player_id, const std::string &token, uint64_t generation);

    size_t pending() const { return pending_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t executed() const { return executed_.load(std::memory_order_relaxed); }

    /** 单测：替换实际执行体（可注入 sleep）；nullptr 恢复默认 SessionStore 路径 */
    void SetExecutorForTest(std::function<void(const Task &)> exec);

private:
    GatewayDisconnectAsync() = default;
    void WorkerLoop();
    void RunDefault(const Task &t);

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task> q_;
    size_t max_queue_ = 4096;
    bool stop_ = true;
    std::thread worker_;
    std::function<void(const Task &)> test_exec_;
    std::atomic<size_t> pending_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> executed_{0};
};
