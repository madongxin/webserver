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
    started_ = false;
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
    Shard *shard = shards_[ShardIndex(player_id, shards_.size())].get();
    {
        std::lock_guard<std::mutex> lk(shard->mu);
        if (shard->q.size() >= max_per_shard_)
            return false;
        shard->q.push_back(std::move(task));
        pending_global_.fetch_add(1, std::memory_order_relaxed);
    }
    shard->cv.notify_one();
    return true;
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
        s->cv.wait(lk, [&]() { return s->q.empty() && s->inflight == 0; });
    }
}
