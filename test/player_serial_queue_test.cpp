/**
 * @file player_serial_queue_test.cpp
 * @brief 验证同一 player_id 任务严格有序；不同 shard 可并行
 */

#include "PlayerSerialQueue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

bool TestSamePlayerOrdered() {
    auto &q = PlayerSerialQueue::Instance();
    q.Stop();
    q.Start(4);

    constexpr int kN = 200;
    std::vector<int> order;
    order.reserve(kN);
    std::mutex order_mu;

    for (int i = 0; i < kN; ++i) {
        q.Post(42, [i, &order, &order_mu]() {
            std::lock_guard<std::mutex> lk(order_mu);
            order.push_back(i);
        });
    }
    q.DrainForTest();

    if (static_cast<int>(order.size()) != kN) {
        std::printf("FAIL size=%zu want=%d\n", order.size(), kN);
        return false;
    }
    for (int i = 0; i < kN; ++i) {
        if (order[static_cast<size_t>(i)] != i) {
            std::printf("FAIL order[%d]=%d\n", i, order[static_cast<size_t>(i)]);
            return false;
        }
    }
    std::printf("OK same player ordered n=%d\n", kN);
    return true;
}

bool TestDifferentPlayersProgress() {
    auto &q = PlayerSerialQueue::Instance();
    q.Stop();
    q.Start(4);

    std::atomic<int> done{0};
    constexpr int kPlayers = 32;
    constexpr int kEach = 20;
    for (int p = 0; p < kPlayers; ++p) {
        for (int i = 0; i < kEach; ++i) {
            q.Post(static_cast<uint64_t>(p), [&done]() { done.fetch_add(1); });
        }
    }
    q.DrainForTest();
    const int want = kPlayers * kEach;
    if (done.load() != want) {
        std::printf("FAIL done=%d want=%d\n", done.load(), want);
        return false;
    }
    std::printf("OK multi-player posts=%d\n", want);
    return true;
}

bool TestOverloadReject() {
    auto &q = PlayerSerialQueue::Instance();
    q.Stop();
    q.SetLimits(2, 8);
    q.Start(1);

    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    std::atomic<bool> holding{false};
    if (!q.TryPost(1, [&]() {
            holding.store(true);
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&]() { return release; });
        })) {
        std::printf("FAIL first TryPost\n");
        return false;
    }
    for (int i = 0; i < 200 && !holding.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!holding.load()) {
        std::printf("FAIL worker did not take blocker\n");
        return false;
    }
    // worker 占用第 1 个；队列还可再塞 max_per_shard=2
    if (!q.TryPost(1, []() {}) || !q.TryPost(1, []() {})) {
        std::printf("FAIL fill to capacity\n");
        {
            std::lock_guard<std::mutex> lk(mu);
            release = true;
        }
        cv.notify_all();
        q.DrainForTest();
        return false;
    }
    const bool rejected = !q.TryPost(1, []() {});
    {
        std::lock_guard<std::mutex> lk(mu);
        release = true;
    }
    cv.notify_all();
    q.DrainForTest();
    if (!rejected) {
        std::printf("FAIL expected TryPost reject when shard full\n");
        return false;
    }
    std::printf("OK overload reject\n");
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    ok = TestSamePlayerOrdered() && ok;
    ok = TestDifferentPlayersProgress() && ok;
    ok = TestOverloadReject() && ok;
    PlayerSerialQueue::Instance().Stop();
    if (!ok) {
        std::printf("FAIL player_serial_queue_test\n");
        return 1;
    }
    std::printf("PASS player_serial_queue_test\n");
    return 0;
}
