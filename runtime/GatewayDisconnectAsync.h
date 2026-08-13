#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/**
 * Gateway 断线本地 Redis 补偿：禁止在 Reactor 线程同步 MarkDisconnected。
 * 有界队列 + 后台 worker；满则丢弃并记指标，依赖 Session TTL 收敛。
 * worker 只捕获独立 State：Stop 超时后可放弃旧 State 而不 UAF。
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
    /** 有界等待排空后停 worker；超时放弃旧 State 并立即返回 */
    void Stop(std::chrono::milliseconds drain_deadline = std::chrono::milliseconds(2000));

    /** Reactor 线程：仅入队；成功 true，满/未启动 false */
    bool EnqueueMarkDisconnected(uint64_t player_id, const std::string &token, uint64_t generation);

    size_t pending() const;
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t executed() const;

    /** 单测：替换实际执行体（可注入 sleep）；nullptr 恢复默认 SessionStore 路径 */
    void SetExecutorForTest(std::function<void(const Task &)> exec);

private:
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<Task> q;
        size_t max_queue = 4096;
        std::atomic<bool> stop{true};
        std::atomic<size_t> pending{0};
        std::atomic<uint64_t> executed{0};
        std::function<void(const Task &)> test_exec;
    };

    GatewayDisconnectAsync() = default;
    ~GatewayDisconnectAsync();

    static void WorkerLoop(std::shared_ptr<State> st);
    static void RunDefault(const Task &t);

    mutable std::mutex life_mu_;
    std::shared_ptr<State> state_;
    std::thread worker_;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> executed_{0};
};
