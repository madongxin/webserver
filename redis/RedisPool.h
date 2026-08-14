#pragma once

#include "RedisClient.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * 有上限的 hiredis 连接池。每个 RedisClient 仍非线程安全；
 * 通过 Lease RAII 保证同一连接同一时刻只被一个调用方使用。
 *
 * CLOSING 后拒绝新 Acquire；Lease 持有 shared_ptr<PoolState>+generation，
 * Shutdown 后旧 Lease 不得把悬空指针放回新 Pool。
 */
class RedisPool {
public:
    struct PoolState {
        std::mutex mu;
        std::condition_variable cv;
        std::vector<std::unique_ptr<RedisClient>> owned;
        std::vector<RedisClient *> free;
        std::string host;
        std::string password;
        int port = 6379;
        bool ready = false;
        bool closing = false;
        uint64_t generation = 0;
        int active = 0;
    };

    static RedisPool &Instance();

    bool Init(const std::string &host, int port, const std::string &password, int pool_size = 8);
    void Shutdown();
    void Shutdown(std::chrono::milliseconds deadline);
    bool ready() const;
    int active_leases() const;
    uint64_t generation() const;

    class Lease {
    public:
        Lease() = default;
        Lease(std::shared_ptr<PoolState> st, RedisClient *c, uint64_t generation);
        ~Lease();
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&o) noexcept;
        Lease &operator=(Lease &&o) noexcept;

        RedisClient *get() const { return client_; }
        RedisClient *operator->() const { return client_; }
        explicit operator bool() const { return client_ != nullptr; }

    private:
        void Release();
        std::shared_ptr<PoolState> state_;
        RedisClient *client_ = nullptr;
        uint64_t generation_ = 0;
    };

    Lease Acquire(int timeout_ms = 3000);

private:
    RedisPool() = default;
    static void ReturnToState(const std::shared_ptr<PoolState> &st, RedisClient *c, uint64_t gen);

    mutable std::mutex life_mu_;
    std::shared_ptr<PoolState> state_;
    uint64_t next_generation_ = 0;
};
