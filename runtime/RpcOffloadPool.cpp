#include "RpcOffloadPool.h"

#include "Logging.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr size_t kMaxPending = 256;

class Pool {
public:
    static Pool &Get() {
        static Pool g;
        return g;
    }

    void Start(int n) {
        std::lock_guard<std::mutex> lk(life_mu_);
        if (started_.load(std::memory_order_relaxed))
            return;
        if (n <= 0) {
            unsigned hc = std::thread::hardware_concurrency();
            n = hc > 4 ? static_cast<int>(hc) : 8;
        }
        if (n < 4)
            n = 4;
        stop_.store(false, std::memory_order_release);
        workers_.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            workers_.emplace_back([this]() { Worker(); });
        started_.store(true, std::memory_order_release);
        LOG_INFO << "RpcOffloadPool started workers=" << n;
    }

    void Stop() {
        std::lock_guard<std::mutex> lk(life_mu_);
        if (!started_.load(std::memory_order_relaxed))
            return;
        stop_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> qlk(mu_);
            cv_.notify_all();
        }
        for (auto &t : workers_) {
            if (t.joinable())
                t.join();
        }
        workers_.clear();
        {
            std::lock_guard<std::mutex> qlk(mu_);
            q_.clear();
        }
        started_.store(false, std::memory_order_release);
        LOG_INFO << "RpcOffloadPool stopped";
    }

    bool TryPost(std::function<void()> fn) {
        if (!started_.load(std::memory_order_acquire))
            Start(0);
        if (!started_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire))
            return false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_.size() >= kMaxPending)
                return false;
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
        return true;
    }

    bool started() const { return started_.load(std::memory_order_acquire); }

private:
    Pool() = default;
    ~Pool() { Stop(); }

    void Worker() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&]() {
                    return stop_.load(std::memory_order_relaxed) || !q_.empty();
                });
                if (stop_.load(std::memory_order_relaxed) && q_.empty())
                    return;
                if (q_.empty())
                    continue;
                fn = std::move(q_.front());
                q_.pop_front();
            }
            try {
                if (fn)
                    fn();
            } catch (...) {
                LOG_ERROR << "RpcOffloadPool: task threw";
            }
        }
    }

    std::mutex life_mu_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{true};
    std::atomic<bool> started_{false};
};

}  // namespace

RpcOffloadPool &RpcOffloadPool::Instance() {
    static RpcOffloadPool g;
    return g;
}

RpcOffloadPool::~RpcOffloadPool() = default;

void RpcOffloadPool::Start(int n) {
    Pool::Get().Start(n);
}

void RpcOffloadPool::Stop() {
    Pool::Get().Stop();
}

bool RpcOffloadPool::TryPost(std::function<void()> fn) {
    return Pool::Get().TryPost(std::move(fn));
}

bool RpcOffloadPool::started() const {
    return Pool::Get().started();
}
