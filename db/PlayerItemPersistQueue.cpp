/**
 * @file PlayerItemPersistQueue.cpp
 * @brief 道具落库队列：入队、定时刷在线玩家、登出即时刷
 */

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
    // 登出不等待 5 分钟，避免道具只存在于内存/队列
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
    // 避免阻塞 EventLoop：刷盘在后台线程执行
    std::thread([this]() { FlushOnlinePlayers(); }).detach();
}

void PlayerItemPersistQueue::FlushOnlinePlayers() {
    if (!ConnectionPool::getconnectionPool()->isInitialized())
        return;
    PlayerItemStore::Instance().EnsureTable();

    // 将队列拆成 to_flush（在线）与 keep（离线）；离线项继续留在 queue_ 直到玩家再次上线
    std::deque<PendingPlayerItem> to_flush;
    std::deque<PendingPlayerItem> keep;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &e : queue_) {
            bool online = false;
#ifdef WEBSERVER_ENABLE_REDIS
            // Redis 有 game:session:{uid} 则以 SessionStore 为准；否则用 grant/login 写入的 online_local_
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
            // INSERT 失败则重新入队，下次定时或登出再试
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back(e);
        }
    }
    LOG_INFO << "PlayerItemPersistQueue: periodic flush online batch=" << to_flush.size()
             << " ok=" << ok << " fail=" << fail << " remain_queue=" << QueueSize();
}

/** 登出或 MarkOffline：将该玩家在 queue_ 中的全部待写记录立即 INSERT */
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
