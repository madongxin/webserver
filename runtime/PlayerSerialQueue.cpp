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
    LOG_INFO << "PlayerSerialQueue started shards=" << shard_count;
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
        {
            std::lock_guard<std::mutex> lk(shard->mu);
            --shard->inflight;
            if (shard->q.empty() && shard->inflight == 0)
                shard->cv.notify_all();
        }
    }
}

void PlayerSerialQueue::Post(uint64_t player_id, std::function<void()> task) {
    if (!started_) {
        // 未 Start 时同步执行，避免测试/早期路径丢请求
        if (task)
            task();
        return;
    }
    Shard *shard = shards_[ShardIndex(player_id, shards_.size())].get();
    {
        std::lock_guard<std::mutex> lk(shard->mu);
        const size_t depth = shard->q.size();
        if (depth > 1000) {
            LOG_WARN << "PlayerSerialQueue shard depth=" << depth
                     << " player_id=" << player_id;
        }
        shard->q.push_back(std::move(task));
    }
    shard->cv.notify_one();
}

void PlayerSerialQueue::DrainForTest() {
    if (!started_)
        return;
    for (auto &s : shards_) {
        std::unique_lock<std::mutex> lk(s->mu);
        s->cv.wait(lk, [&]() { return s->q.empty() && s->inflight == 0; });
    }
}
