#include "GatewayDisconnectAsync.h"

#include "Logging.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "SessionStore.h"
#endif

GatewayDisconnectAsync &GatewayDisconnectAsync::Instance() {
    static GatewayDisconnectAsync inst;
    return inst;
}

void GatewayDisconnectAsync::Start(size_t max_queue) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!stop_)
        return;
    max_queue_ = max_queue > 0 ? max_queue : 4096;
    stop_ = false;
    pending_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    executed_.store(0, std::memory_order_relaxed);
    worker_ = std::thread([this] { WorkerLoop(); });
}

void GatewayDisconnectAsync::Stop(std::chrono::milliseconds drain_deadline) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_)
            return;
        stop_ = true;
    }
    cv_.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + drain_deadline;
    while (pending_.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (worker_.joinable())
        worker_.join();
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
    pending_.store(0, std::memory_order_relaxed);
    test_exec_ = nullptr;
}

bool GatewayDisconnectAsync::EnqueueMarkDisconnected(uint64_t player_id, const std::string &token,
                                                     uint64_t generation) {
    if (player_id == 0)
        return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_)
            return false;
        if (q_.size() >= max_queue_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN << "GatewayDisconnectAsync queue full drop player=" << player_id
                     << " dropped=" << dropped_.load(std::memory_order_relaxed);
            return false;
        }
        Task t;
        t.player_id = player_id;
        t.token = token;
        t.generation = generation;
        q_.push_back(std::move(t));
        pending_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
    return true;
}

void GatewayDisconnectAsync::SetExecutorForTest(std::function<void(const Task &)> exec) {
    std::lock_guard<std::mutex> lk(mu_);
    test_exec_ = std::move(exec);
}

void GatewayDisconnectAsync::RunDefault(const Task &t) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (SessionStore::Instance().Available())
        SessionStore::Instance().MarkDisconnected(t.player_id, t.token, t.generation);
#else
    (void)t;
#endif
}

void GatewayDisconnectAsync::WorkerLoop() {
    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            if (q_.empty()) {
                if (stop_)
                    return;
                continue;
            }
            t = std::move(q_.front());
            q_.pop_front();
        }
        std::function<void(const Task &)> exec;
        {
            std::lock_guard<std::mutex> lk(mu_);
            exec = test_exec_;
        }
        try {
            if (exec)
                exec(t);
            else
                RunDefault(t);
            executed_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            LOG_WARN << "GatewayDisconnectAsync exec exception player=" << t.player_id;
        }
        pending_.fetch_sub(1, std::memory_order_relaxed);
    }
}
