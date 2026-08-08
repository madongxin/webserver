#include "RedisPool.h"

#include "Logging.h"

#include <chrono>

RedisPool &RedisPool::Instance() {
    static RedisPool g;
    return g;
}

bool RedisPool::Init(const std::string &host, int port, const std::string &password, int pool_size) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ready_)
        return true;
    if (pool_size < 1)
        pool_size = 1;
    if (pool_size > 64)
        pool_size = 64;
    host_ = host;
    port_ = port;
    password_ = password;
    owned_.clear();
    free_.clear();
    owned_.reserve(static_cast<size_t>(pool_size));
    for (int i = 0; i < pool_size; ++i) {
        auto c = std::make_unique<RedisClient>();
        if (!c->Connect(host, port, password)) {
            LOG_ERROR << "RedisPool: connect failed " << host << ":" << port << " idx=" << i;
            owned_.clear();
            free_.clear();
            ready_ = false;
            return false;
        }
        free_.push_back(c.get());
        owned_.push_back(std::move(c));
    }
    ready_ = true;
    LOG_INFO << "RedisPool ready size=" << pool_size << " " << host << ":" << port;
    return true;
}

void RedisPool::Shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    free_.clear();
    owned_.clear();
    ready_ = false;
}

bool RedisPool::ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ready_;
}

RedisPool::Lease::Lease(RedisClient *c) : client_(c) {}

RedisPool::Lease::~Lease() {
    if (client_)
        RedisPool::Instance().Release(client_);
    client_ = nullptr;
}

RedisPool::Lease::Lease(Lease &&o) noexcept : client_(o.client_) {
    o.client_ = nullptr;
}

RedisPool::Lease &RedisPool::Lease::operator=(Lease &&o) noexcept {
    if (this != &o) {
        if (client_)
            RedisPool::Instance().Release(client_);
        client_ = o.client_;
        o.client_ = nullptr;
    }
    return *this;
}

RedisPool::Lease RedisPool::Acquire(int timeout_ms) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!ready_)
        return Lease();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (free_.empty()) {
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            LOG_ERROR << "RedisPool: acquire timeout";
            return Lease();
        }
        if (!ready_)
            return Lease();
    }
    RedisClient *c = free_.back();
    free_.pop_back();
    if (!c->IsConnected() || !c->Ping()) {
        if (!c->Connect(host_, port_, password_)) {
            free_.push_back(c);
            LOG_ERROR << "RedisPool: reconnect failed";
            return Lease();
        }
    }
    return Lease(c);
}

void RedisPool::Release(RedisClient *c) {
    if (!c)
        return;
    std::lock_guard<std::mutex> lk(mu_);
    free_.push_back(c);
    cv_.notify_one();
}
