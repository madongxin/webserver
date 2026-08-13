#include "GatewayDisconnectAsync.h"

#include "Logging.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "SessionStore.h"
#endif

GatewayDisconnectAsync &GatewayDisconnectAsync::Instance() {
    static GatewayDisconnectAsync inst;
    return inst;
}

GatewayDisconnectAsync::~GatewayDisconnectAsync() {
    Stop(std::chrono::milliseconds(2000));
}

void GatewayDisconnectAsync::Start(size_t max_queue) {
    std::lock_guard<std::mutex> lk(life_mu_);
    if (state_ && !state_->stop.load(std::memory_order_acquire))
        return;
    auto st = std::make_shared<State>();
    st->max_queue = max_queue > 0 ? max_queue : 4096;
    st->stop.store(false, std::memory_order_release);
    state_ = st;
    worker_ = std::thread([st] { WorkerLoop(st); });
}

void GatewayDisconnectAsync::Stop(std::chrono::milliseconds drain_deadline) {
    std::shared_ptr<State> st;
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        st = state_;
        if (!st)
            return;
        if (st->stop.load(std::memory_order_acquire) && !worker_.joinable()) {
            state_.reset();
            return;
        }
        st->stop.store(true, std::memory_order_release);
        st->cv.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + drain_deadline;
    while (st->pending.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::lock_guard<std::mutex> lk(life_mu_);
    if (worker_.joinable()) {
        if (st->pending.load(std::memory_order_relaxed) == 0) {
            worker_.join();
        } else {
            worker_.detach();
            worker_ = std::thread();
        }
    }
    executed_.fetch_add(st->executed.load(std::memory_order_relaxed), std::memory_order_relaxed);
    st->executed.store(0, std::memory_order_relaxed);
    if (state_ == st)
        state_.reset();
}

size_t GatewayDisconnectAsync::pending() const {
    std::lock_guard<std::mutex> lk(life_mu_);
    return state_ ? state_->pending.load(std::memory_order_relaxed) : 0;
}

uint64_t GatewayDisconnectAsync::executed() const {
    std::lock_guard<std::mutex> lk(life_mu_);
    const uint64_t live = state_ ? state_->executed.load(std::memory_order_relaxed) : 0;
    return executed_.load(std::memory_order_relaxed) + live;
}

bool GatewayDisconnectAsync::EnqueueMarkDisconnected(uint64_t player_id, const std::string &token,
                                                     uint64_t generation) {
    if (player_id == 0)
        return false;
    std::shared_ptr<State> st;
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        st = state_;
    }
    if (!st || st->stop.load(std::memory_order_acquire))
        return false;
    {
        std::lock_guard<std::mutex> lk(st->mu);
        if (st->stop.load(std::memory_order_acquire))
            return false;
        if (st->q.size() >= st->max_queue) {
            const uint64_t n = dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || (n % 64) == 0) {
                LOG_WARN << "GatewayDisconnectAsync queue full drop player=" << player_id
                         << " dropped=" << n;
            }
            return false;
        }
        Task t;
        t.player_id = player_id;
        t.token = token;
        t.generation = generation;
        st->q.push_back(std::move(t));
        st->pending.fetch_add(1, std::memory_order_relaxed);
    }
    st->cv.notify_one();
    return true;
}

void GatewayDisconnectAsync::SetExecutorForTest(std::function<void(const Task &)> exec) {
    std::lock_guard<std::mutex> lk(life_mu_);
    if (!state_)
        return;
    std::lock_guard<std::mutex> qlk(state_->mu);
    state_->test_exec = std::move(exec);
}

void GatewayDisconnectAsync::RunDefault(const Task &t) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (SessionStore::Instance().Available())
        SessionStore::Instance().MarkDisconnected(t.player_id, t.token, t.generation);
#else
    (void)t;
#endif
}

void GatewayDisconnectAsync::WorkerLoop(std::shared_ptr<State> st) {
    for (;;) {
        Task t;
        {
            std::unique_lock<std::mutex> lk(st->mu);
            st->cv.wait(lk, [&] { return st->stop.load(std::memory_order_acquire) || !st->q.empty(); });
            if (st->q.empty()) {
                if (st->stop.load(std::memory_order_acquire))
                    return;
                continue;
            }
            t = std::move(st->q.front());
            st->q.pop_front();
        }
        std::function<void(const Task &)> exec;
        {
            std::lock_guard<std::mutex> lk(st->mu);
            exec = st->test_exec;
        }
        try {
            if (exec)
                exec(t);
            else
                RunDefault(t);
            st->executed.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            LOG_WARN << "GatewayDisconnectAsync exec exception player=" << t.player_id;
        }
        st->pending.fetch_sub(1, std::memory_order_relaxed);
    }
}
