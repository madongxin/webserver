#include "RedisPool.h"

#include "Logging.h"

#include <chrono>

RedisPool &RedisPool::Instance() {
    static RedisPool g;
    return g;
}

bool RedisPool::Init(const std::string &host, int port, const std::string &password, int pool_size) {
    std::lock_guard<std::mutex> lk(life_mu_);
    if (state_ && state_->ready && !state_->closing)
        return true;
    if (pool_size < 1)
        pool_size = 1;
    if (pool_size > 64)
        pool_size = 64;
    auto st = std::make_shared<PoolState>();
    st->host = host;
    st->port = port;
    st->password = password;
    st->generation = ++next_generation_;
    st->owned.reserve(static_cast<size_t>(pool_size));
    for (int i = 0; i < pool_size; ++i) {
        auto c = std::make_unique<RedisClient>();
        if (!c->Connect(host, port, password)) {
            LOG_ERROR << "RedisPool: connect failed " << host << ":" << port << " idx=" << i;
            return false;
        }
        st->free.push_back(c.get());
        st->owned.push_back(std::move(c));
    }
    st->ready = true;
    st->closing = false;
    state_ = std::move(st);
    LOG_INFO << "RedisPool ready size=" << pool_size << " " << host << ":" << port
             << " generation=" << state_->generation;
    return true;
}

void RedisPool::Shutdown() {
    Shutdown(std::chrono::milliseconds(3000));
}

void RedisPool::Shutdown(std::chrono::milliseconds deadline) {
    std::shared_ptr<PoolState> st;
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        st = state_;
        if (!st)
            return;
        {
            std::lock_guard<std::mutex> slk(st->mu);
            st->closing = true;
            st->ready = false;
            st->cv.notify_all();
        }
    }
    const auto until = std::chrono::steady_clock::now() + deadline;
    {
        std::unique_lock<std::mutex> slk(st->mu);
        while (st->active > 0 && std::chrono::steady_clock::now() < until)
            st->cv.wait_until(slk, until);
        if (st->active > 0) {
            LOG_WARN << "RedisPool Shutdown timeout active_leases=" << st->active
                     << " generation=" << st->generation;
        }
        st->free.clear();
        if (st->active <= 0)
            st->owned.clear();
    }
    std::lock_guard<std::mutex> lk(life_mu_);
    if (state_ == st)
        state_.reset();
}

bool RedisPool::ready() const {
    std::lock_guard<std::mutex> lk(life_mu_);
    return state_ && state_->ready && !state_->closing;
}

int RedisPool::active_leases() const {
    std::lock_guard<std::mutex> lk(life_mu_);
    if (!state_)
        return 0;
    std::lock_guard<std::mutex> slk(state_->mu);
    return state_->active;
}

uint64_t RedisPool::generation() const {
    std::lock_guard<std::mutex> lk(life_mu_);
    return state_ ? state_->generation : 0;
}

void RedisPool::ReturnToState(const std::shared_ptr<PoolState> &st, RedisClient *c, uint64_t gen) {
    if (!st || !c)
        return;
    std::lock_guard<std::mutex> lk(st->mu);
    if (st->active > 0)
        --st->active;
    if (!st->closing && gen == st->generation && st->ready) {
        if (!c->IsConnected()) {
            if (!c->Connect(st->host, st->port, st->password)) {
                LOG_WARN << "RedisPool: reconnect on release failed " << st->host << ":" << st->port;
            }
        }
        st->free.push_back(c);
    }
    st->cv.notify_one();
}

RedisPool::Lease::Lease(std::shared_ptr<PoolState> st, RedisClient *c, uint64_t generation)
    : state_(std::move(st)), client_(c), generation_(generation) {}

RedisPool::Lease::~Lease() {
    Release();
}

void RedisPool::Lease::Release() {
    if (client_)
        RedisPool::ReturnToState(state_, client_, generation_);
    client_ = nullptr;
    state_.reset();
    generation_ = 0;
}

RedisPool::Lease::Lease(Lease &&o) noexcept
    : state_(std::move(o.state_)), client_(o.client_), generation_(o.generation_) {
    o.client_ = nullptr;
    o.generation_ = 0;
}

RedisPool::Lease &RedisPool::Lease::operator=(Lease &&o) noexcept {
    if (this != &o) {
        Release();
        state_ = std::move(o.state_);
        client_ = o.client_;
        generation_ = o.generation_;
        o.client_ = nullptr;
        o.generation_ = 0;
    }
    return *this;
}

RedisPool::Lease RedisPool::Acquire(int timeout_ms) {
    std::shared_ptr<PoolState> st;
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        st = state_;
    }
    if (!st)
        return Lease();
    std::unique_lock<std::mutex> lk(st->mu);
    if (!st->ready || st->closing)
        return Lease();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (st->free.empty()) {
        if (st->cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            LOG_ERROR << "RedisPool: acquire timeout";
            return Lease();
        }
        if (!st->ready || st->closing)
            return Lease();
    }
    RedisClient *c = st->free.back();
    st->free.pop_back();
    if (!c->IsConnected() || !c->Ping()) {
        if (!c->Connect(st->host, st->port, st->password)) {
            st->free.push_back(c);
            LOG_ERROR << "RedisPool: reconnect failed";
            return Lease();
        }
    }
    ++st->active;
    const uint64_t gen = st->generation;
    lk.unlock();
    return Lease(std::move(st), c, gen);
}
