#include "PlayerItemPersistQueue.h"

#include "ConnectionPool.h"
#include "EventLoop.h"
#include "Logging.h"
#include "PlayerItemStore.h"

#ifdef WEBSERVER_ENABLE_REDIS
#include "SessionStore.h"
#endif

#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr double kDefaultFlushIntervalSec = 300.0;

}  // namespace

PlayerItemPersistQueue &PlayerItemPersistQueue::Instance() {
    static PlayerItemPersistQueue g;
    return g;
}

void PlayerItemPersistQueue::StartPeriodic(EventLoop *loop, double interval_sec) {
    if (!loop || interval_sec <= 0.0)
        return;
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        LOG_WARN << "PlayerItemPersistQueue: MySQL pool not ready, skip periodic flush";
        return;
    }
    PlayerItemStore::Instance().EnsureTable();
    LOG_INFO << "PlayerItemPersistQueue: flush online players every " << interval_sec << "s";
    loop->RunEvery(interval_sec, [this]() { OnFlushTick(); });
}

void PlayerItemPersistQueue::MarkOnline(uint64_t player_id) {
    if (player_id == 0)
        return;
    std::lock_guard<std::mutex> lk(mu_);
    online_local_.insert(player_id);
}

void PlayerItemPersistQueue::MarkOffline(uint64_t player_id) {
    if (player_id == 0)
        return;
    FlushPlayer(player_id);
    std::lock_guard<std::mutex> lk(mu_);
    online_local_.erase(player_id);
}

void PlayerItemPersistQueue::Enqueue(const PendingPlayerItem &item) {
    if (item.player_id == 0 || item.item_id == 0 || item.count == 0)
        return;
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(item);
    online_local_.insert(item.player_id);
    LOG_INFO << "PlayerItemPersistQueue: enqueue player_id=" << item.player_id
             << " item_id=" << item.item_id << " count=" << item.count
             << " queue_size=" << queue_.size();
}

size_t PlayerItemPersistQueue::QueueSize() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size();
}

void PlayerItemPersistQueue::OnFlushTick() {
    std::thread([this]() { FlushOnlinePlayers(); }).detach();
}

void PlayerItemPersistQueue::FlushOnlinePlayers() {
    if (!ConnectionPool::getconnectionPool()->isInitialized())
        return;
    PlayerItemStore::Instance().EnsureTable();

    std::deque<PendingPlayerItem> to_flush;
    std::deque<PendingPlayerItem> keep;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &e : queue_) {
            bool online = false;
#ifdef WEBSERVER_ENABLE_REDIS
            if (SessionStore::Instance().Available())
                online = SessionStore::Instance().IsPlayerOnline(e.player_id);
            else
                online = online_local_.count(e.player_id) > 0;
#else
            online = online_local_.count(e.player_id) > 0;
#endif
            if (online)
                to_flush.push_back(e);
            else
                keep.push_back(e);
        }
        queue_.swap(keep);
    }

    size_t ok = 0;
    size_t fail = 0;
    for (const auto &e : to_flush) {
        uint64_t instance_id = 0;
        if (PlayerItemStore::Instance().Insert(e.player_id, e.item_id, e.count, e.expire_time_sec,
                                               e.extra_data, &instance_id)) {
            ++ok;
        } else {
            ++fail;
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back(e);
        }
    }
    LOG_INFO << "PlayerItemPersistQueue: periodic flush online batch=" << to_flush.size()
             << " ok=" << ok << " fail=" << fail << " remain_queue=" << QueueSize();
}

void PlayerItemPersistQueue::FlushPlayer(uint64_t player_id) {
    if (player_id == 0 || !ConnectionPool::getconnectionPool()->isInitialized())
        return;

    std::deque<PendingPlayerItem> to_flush;
    std::deque<PendingPlayerItem> keep;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &e : queue_) {
            if (e.player_id == player_id)
                to_flush.push_back(e);
            else
                keep.push_back(e);
        }
        queue_.swap(keep);
    }
    if (to_flush.empty())
        return;

    size_t ok = 0;
    for (const auto &e : to_flush) {
        uint64_t instance_id = 0;
        if (PlayerItemStore::Instance().Insert(e.player_id, e.item_id, e.count, e.expire_time_sec,
                                               e.extra_data, &instance_id))
            ++ok;
        else {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back(e);
        }
    }
    LOG_INFO << "PlayerItemPersistQueue: logout flush player_id=" << player_id << " rows=" << ok;
}
