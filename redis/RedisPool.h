#pragma once

#include "RedisClient.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * 有上限的 hiredis 连接池。每个 RedisClient 仍非线程安全；
 * 通过 Lease RAII 保证同一连接同一时刻只被一个调用方使用。
 */
class RedisPool {
public:
    static RedisPool &Instance();

    bool Init(const std::string &host, int port, const std::string &password, int pool_size = 8);
    void Shutdown();
    bool ready() const;

    class Lease {
    public:
        Lease() = default;
        explicit Lease(RedisClient *c);
        ~Lease();
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&o) noexcept;
        Lease &operator=(Lease &&o) noexcept;

        RedisClient *get() const { return client_; }
        RedisClient *operator->() const { return client_; }
        explicit operator bool() const { return client_ != nullptr; }

    private:
        RedisClient *client_ = nullptr;
    };

    Lease Acquire(int timeout_ms = 3000);

private:
    RedisPool() = default;
    void Release(RedisClient *c);

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<RedisClient>> owned_;
    std::vector<RedisClient *> free_;
    bool ready_ = false;
    std::string host_;
    int port_ = 6379;
    std::string password_;
};
