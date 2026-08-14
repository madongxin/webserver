/**
 * PlayerSerialQueue::Stop(deadline) 协作取消：可取消长任务必须在时限内返回且可再次 Start。
 */
#include "PlayerSerialQueue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    auto &q = PlayerSerialQueue::Instance();
    q.Stop();
    q.Start(1);

    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> saw_stop{false};
    if (!q.TryPost(7, [&]() {
            started.store(true);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < end) {
                if (PlayerSerialQueue::Instance().stop_requested()) {
                    saw_stop.store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            finished.store(true);
        })) {
        std::printf("FAIL post\n");
        q.Stop();
        return 1;
    }
    for (int i = 0; i < 200 && !started.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!started.load()) {
        std::printf("FAIL task not started\n");
        q.Stop();
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto rc = q.Stop(std::chrono::milliseconds(200));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (ms > 800) {
        std::printf("FAIL Stop elapsed_ms=%lld (limit 800)\n", static_cast<long long>(ms));
        return 1;
    }
    if (!saw_stop.load() || !finished.load()) {
        std::printf("FAIL cooperative cancel saw_stop=%d finished=%d\n", saw_stop.load(),
                    finished.load());
        return 1;
    }
    (void)rc;

    q.Start(1);
    std::atomic<int> ran{0};
    if (!q.TryPost(8, [&]() { ran.fetch_add(1); })) {
        std::printf("FAIL restart post\n");
        q.Stop();
        return 1;
    }
    q.DrainForTest();
    if (ran.load() != 1) {
        std::printf("FAIL restart execute\n");
        q.Stop();
        return 1;
    }
    q.Stop();
    std::printf("OK player_serial_shutdown_test elapsed_ms=%lld\n", static_cast<long long>(ms));
    return 0;
}
