#include "PlayerSerialQueue.h"

#include "Logging.h"

#include <utility>

PlayerSerialQueue &PlayerSerialQueue::Instance() {
    static PlayerSerialQueue g;
    return g;
}

PlayerSerialQueue::~PlayerSerialQueue() {
    Stop();
}

size_t PlayerSerialQueue::ShardIndex(uint64_t player_id, size_t n) {
    if (n == 0)
        return 0;
    return static_cast<size_t>(player_id % n);
}

PlayerSerialQueue::Shard *PlayerSerialQueue::ShardFor(uint64_t player_id) {
    const auto s = life_.load(std::memory_order_acquire);
    if ((s != LifeState::kRunning && s != LifeState::kDraining) || shards_.empty())
        return nullptr;
    return shards_[ShardIndex(player_id, shards_.size())].get();
}

void PlayerSerialQueue::SetLimits(size_t max_per_shard, size_t max_global) {
    if (max_per_shard > 0)
        max_per_shard_ = max_per_shard;
    if (max_global > 0)
        max_global_ = max_global;
}

void PlayerSerialQueue::FinishAsyncLevelLocked(Shard *shard, uint64_t player_id) {
    auto it = shard->async_depth.find(player_id);
    if (it == shard->async_depth.end()) {
        auto dit = shard->deferred.find(player_id);
        if (dit != shard->deferred.end()) {
            while (!dit->second.empty()) {
                shard->q.push_back(std::move(dit->second.front()));
                dit->second.pop_front();
            }
            shard->deferred.erase(dit);
        }
        return;
    }
    if (--(it->second) > 0)
        return;
    shard->async_depth.erase(it);
    auto dit = shard->deferred.find(player_id);
    if (dit != shard->deferred.end()) {
        while (!dit->second.empty()) {
            shard->q.push_back(std::move(dit->second.front()));
            dit->second.pop_front();
        }
        shard->deferred.erase(dit);
    }
}

void PlayerSerialQueue::Start(int shard_count) {
    std::lock_guard<std::mutex> lk(life_mu_);
    if (life_.load(std::memory_order_relaxed) != LifeState::kStopped)
        return;
    if (shard_count <= 0) {
        unsigned hc = std::thread::hardware_concurrency();
        shard_count = hc > 1 ? static_cast<int>(hc / 2) : 1;
        if (shard_count < 1)
            shard_count = 1;
    }
    shards_.reserve(static_cast<size_t>(shard_count));
    for (int i = 0; i < shard_count; ++i) {
        auto shard = std::make_unique<Shard>();
        Shard *raw = shard.get();
        shard->worker = std::thread([this, raw]() { WorkerLoop(raw); });
        shards_.push_back(std::move(shard));
    }
    pending_global_.store(0, std::memory_order_relaxed);
    life_.store(LifeState::kRunning, std::memory_order_release);
    LOG_INFO << "PlayerSerialQueue started shards=" << shard_count
             << " max_per_shard=" << max_per_shard_ << " max_global=" << max_global_;
}

void PlayerSerialQueue::WaitUntilDrainedOrDeadline(std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_clear = true;
        size_t pending_players = 0;
        size_t pending_tasks = 0;
        for (auto &s : shards_) {
            std::lock_guard<std::mutex> qlk(s->mu);
            if (!s->q.empty() || s->inflight != 0 || !s->async_depth.empty() ||
                !s->deferred.empty()) {
                all_clear = false;
                pending_players += s->async_depth.size();
                pending_tasks += s->q.size();
                for (const auto &kv : s->deferred)
                    pending_tasks += kv.second.size();
            }
        }
        if (all_clear)
            return;
        (void)pending_players;
        (void)pending_tasks;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    size_t leftover_players = 0;
    size_t leftover_tasks = 0;
    for (auto &s : shards_) {
        std::lock_guard<std::mutex> qlk(s->mu);
        leftover_players += s->async_depth.size();
        leftover_tasks += s->q.size() + static_cast<size_t>(s->inflight);
        for (const auto &kv : s->deferred)
            leftover_tasks += kv.second.size();
    }
    LOG_WARN << "PlayerSerialQueue drain deadline: async_players=" << leftover_players
             << " pending_tasks~=" << leftover_tasks;
}

void PlayerSerialQueue::BeginDrain(std::chrono::milliseconds deadline) {
    {
        std::lock_guard<std::mutex> lk(life_mu_);
        if (life_.load(std::memory_order_relaxed) != LifeState::kRunning)
            return;
        life_.store(LifeState::kDraining, std::memory_order_release);
        LOG_INFO << "PlayerSerialQueue draining";
    }
    WaitUntilDrainedOrDeadline(std::chrono::steady_clock::now() + deadline);
}

void PlayerSerialQueue::Stop() {
    std::unique_lock<std::mutex> lk(life_mu_);
    if (life_.load(std::memory_order_relaxed) == LifeState::kStopped)
        return;
    if (life_.load(std::memory_order_relaxed) == LifeState::kRunning)
        life_.store(LifeState::kDraining, std::memory_order_release);
    lk.unlock();

    WaitUntilDrainedOrDeadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(3000));

    lk.lock();
    if (life_.load(std::memory_order_relaxed) == LifeState::kStopped)
        return;

    // 先切断 Complete/TryPost，避免 join 期间 callback 再改状态
    life_.store(LifeState::kStopped, std::memory_order_release);

    for (auto &s : shards_) {
        {
            std::lock_guard<std::mutex> qlk(s->mu);
            // 超时强制取消：丢掉未完成 deferred/async，避免永久 join
            size_t dropped = 0;
            for (auto &kv : s->deferred) {
                dropped += kv.second.size();
                while (!kv.second.empty()) {
                    kv.second.pop_front();
                    pending_global_.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            s->deferred.clear();
            dropped += s->async_depth.size();
            s->async_depth.clear();
            if (dropped > 0)
                LOG_WARN << "PlayerSerialQueue force-stop dropped~=" << dropped;
            s->stop = true;
        }
        s->cv.notify_all();
    }
    for (auto &s : shards_) {
        if (s->worker.joinable())
            s->worker.join();
    }
    shards_.clear();
    pending_global_.store(0, std::memory_order_relaxed);
    LOG_INFO << "PlayerSerialQueue stopped";
}

void PlayerSerialQueue::WorkerLoop(Shard *shard) {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(shard->mu);
            shard->cv.wait(lk, [&]() {
                if (!shard->q.empty())
                    return true;
                if (!shard->stop)
                    return false;
                return shard->async_depth.empty() && shard->deferred.empty() &&
                       shard->inflight == 0;
            });
            if (shard->stop && shard->q.empty() && shard->async_depth.empty() &&
                shard->deferred.empty() && shard->inflight == 0)
                return;
            if (shard->q.empty())
                continue;
            task = std::move(shard->q.front());
            shard->q.pop_front();
            ++shard->inflight;
        }
        try {
            if (task)
                task();
        } catch (...) {
            LOG_ERROR << "PlayerSerialQueue: task threw";
        }
        pending_global_.fetch_sub(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(shard->mu);
            --shard->inflight;
            if (shard->q.empty() && shard->inflight == 0)
                shard->cv.notify_all();
        }
    }
}

bool PlayerSerialQueue::TryPost(uint64_t player_id, std::function<void()> task) {
    if (life_.load(std::memory_order_acquire) != LifeState::kRunning)
        return false;
    if (pending_global_.load(std::memory_order_relaxed) >= max_global_)
        return false;
    Shard *shard = ShardFor(player_id);
    if (!shard)
        return false;
    {
        std::lock_guard<std::mutex> lk(shard->mu);
        if (shard->stop)
            return false;
        const auto dit = shard->async_depth.find(player_id);
        if (dit != shard->async_depth.end() && dit->second > 0) {
            auto &dq = shard->deferred[player_id];
            if (dq.size() + shard->q.size() >= max_per_shard_)
                return false;
            dq.push_back(std::move(task));
            pending_global_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (shard->q.size() >= max_per_shard_)
            return false;
        shard->q.push_back(std::move(task));
        pending_global_.fetch_add(1, std::memory_order_relaxed);
    }
    shard->cv.notify_one();
    return true;
}

void PlayerSerialQueue::MarkAsyncInFlight(uint64_t player_id) {
    Shard *shard = ShardFor(player_id);
    if (!shard)
        return;
    std::lock_guard<std::mutex> lk(shard->mu);
    shard->async_depth[player_id] += 1;
}

void PlayerSerialQueue::ClearAsyncInFlight(uint64_t player_id) {
    CompleteAsyncInFlight(player_id, nullptr);
}

bool PlayerSerialQueue::CompleteAsyncInFlight(uint64_t player_id,
                                             std::function<void()> completion) {
    Shard *shard = ShardFor(player_id);
    if (!shard) {
        // STOPPED：禁止 inline 执行业务 completion；调用方取消 done
        return false;
    }
    if (!completion) {
        {
            std::lock_guard<std::mutex> lk(shard->mu);
            FinishAsyncLevelLocked(shard, player_id);
        }
        shard->cv.notify_one();
        return true;
    }
    // completion 不受外部 max_global 限制：已开始的异步必须能回投原玩家串行上下文。
    // 关键：先入队 completion，depth 在其执行后再减；链式 Mark 会使 depth 保持 >0，deferred 不提前释放。
    auto wrapped = [this, player_id, completion = std::move(completion)]() mutable {
        if (completion)
            completion();
        Shard *s = ShardFor(player_id);
        if (!s)
            return;
        {
            std::lock_guard<std::mutex> lk(s->mu);
            FinishAsyncLevelLocked(s, player_id);
        }
        s->cv.notify_one();
    };
    {
        std::lock_guard<std::mutex> lk(shard->mu);
        shard->q.push_front(std::move(wrapped));
        pending_global_.fetch_add(1, std::memory_order_relaxed);
    }
    shard->cv.notify_one();
    return true;
}

bool PlayerSerialQueue::IsAsyncInFlight(uint64_t player_id) const {
    const auto s = life_.load(std::memory_order_acquire);
    if ((s != LifeState::kRunning && s != LifeState::kDraining) || shards_.empty())
        return false;
    Shard *shard = shards_[ShardIndex(player_id, shards_.size())].get();
    std::lock_guard<std::mutex> lk(shard->mu);
    const auto it = shard->async_depth.find(player_id);
    return it != shard->async_depth.end() && it->second > 0;
}

void PlayerSerialQueue::Post(uint64_t player_id, std::function<void()> task) {
    if (!TryPost(player_id, std::move(task))) {
        LOG_WARN << "PlayerSerialQueue overload drop player_id=" << player_id
                 << " pending=" << pending_global_.load(std::memory_order_relaxed);
    }
}

void PlayerSerialQueue::DrainForTest() {
    if (!started())
        return;
    for (auto &s : shards_) {
        std::unique_lock<std::mutex> lk(s->mu);
        s->cv.wait(lk, [&]() {
            return s->q.empty() && s->inflight == 0 && s->async_depth.empty() &&
                   s->deferred.empty();
        });
    }
}
