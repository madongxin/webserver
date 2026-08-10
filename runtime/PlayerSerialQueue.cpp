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
    if (!started_ || shards_.empty())
        return nullptr;
    return shards_[ShardIndex(player_id, shards_.size())].get();
}

void PlayerSerialQueue::SetLimits(size_t max_per_shard, size_t max_global) {
    if (max_per_shard > 0)
        max_per_shard_ = max_per_shard;
    if (max_global > 0)
        max_global_ = max_global;
}

void PlayerSerialQueue::Start(int shard_count) {
    std::lock_guard<std::mutex> lk(life_mu_);
    if (started_)
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
    started_ = true;
    pending_global_.store(0, std::memory_order_relaxed);
    LOG_INFO << "PlayerSerialQueue started shards=" << shard_count
             << " max_per_shard=" << max_per_shard_ << " max_global=" << max_global_;
}

void PlayerSerialQueue::Stop() {
    std::lock_guard<std::mutex> lk(life_mu_);
    if (!started_)
        return;
    // 先切断 ShardFor / TryPost，避免异步 Complete 在析构 cv 后仍 notify
    started_ = false;
    for (auto &s : shards_) {
        {
            std::lock_guard<std::mutex> qlk(s->mu);
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
            shard->cv.wait(lk, [&]() { return shard->stop || !shard->q.empty(); });
            if (shard->stop && shard->q.empty())
                return;
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
    if (!started_) {
        if (task)
            task();
        return true;
    }
    if (pending_global_.load(std::memory_order_relaxed) >= max_global_)
        return false;
    Shard *shard = ShardFor(player_id);
    if (!shard)
        return false;
    {
        std::lock_guard<std::mutex> lk(shard->mu);
        if (shard->async_inflight.count(player_id)) {
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
    shard->async_inflight.insert(player_id);
}

void PlayerSerialQueue::ClearAsyncInFlight(uint64_t player_id) {
    CompleteAsyncInFlight(player_id, nullptr);
}

bool PlayerSerialQueue::CompleteAsyncInFlight(uint64_t player_id,
                                               std::function<void()> completion) {
    Shard *shard = ShardFor(player_id);
    if (!shard) {
        if (completion)
            completion();
        return true;
    }
    if (pending_global_.load(std::memory_order_relaxed) >= max_global_ && completion)
        return false;
    {
        std::lock_guard<std::mutex> lk(shard->mu);
        shard->async_inflight.erase(player_id);
        if (completion) {
            shard->q.push_front(std::move(completion));
            pending_global_.fetch_add(1, std::memory_order_relaxed);
        }
        auto it = shard->deferred.find(player_id);
        if (it != shard->deferred.end()) {
            while (!it->second.empty()) {
                shard->q.push_back(std::move(it->second.front()));
                it->second.pop_front();
            }
            shard->deferred.erase(it);
        }
    }
    shard->cv.notify_one();
    return true;
}

bool PlayerSerialQueue::IsAsyncInFlight(uint64_t player_id) const {
    if (!started_ || shards_.empty())
        return false;
    Shard *shard = shards_[ShardIndex(player_id, shards_.size())].get();
    std::lock_guard<std::mutex> lk(shard->mu);
    return shard->async_inflight.count(player_id) > 0;
}

void PlayerSerialQueue::Post(uint64_t player_id, std::function<void()> task) {
    if (!TryPost(player_id, std::move(task))) {
        LOG_WARN << "PlayerSerialQueue overload drop player_id=" << player_id
                 << " pending=" << pending_global_.load(std::memory_order_relaxed);
    }
}

void PlayerSerialQueue::DrainForTest() {
    if (!started_)
        return;
    for (auto &s : shards_) {
        std::unique_lock<std::mutex> lk(s->mu);
        s->cv.wait(lk, [&]() {
            return s->q.empty() && s->inflight == 0 && s->async_inflight.empty() &&
                   s->deferred.empty();
        });
    }
}
