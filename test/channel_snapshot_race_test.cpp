/**
 * 阶段二：Channel 快照并发更新 — 多线程读 + 反复 ApplySnapshot；空快照保留。
 */
#include "BrpcChannelManager.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    auto &mgr = BrpcChannelManager::Instance();
    mgr.Shutdown();
    if (!mgr.Init({"127.0.0.1:19001", "127.0.0.1:19002"}, {"gl-0", "gl-1"}, 50)) {
        std::printf("FAIL init\n");
        return 1;
    }

    std::atomic<int> reads{0};
    std::atomic<int> snaps{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> thr;
    auto held = mgr.SharedChannelForInstance("gl-0");

    for (int t = 0; t < 12; ++t) {
        thr.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                auto c = mgr.SharedChannelForPlayer(static_cast<uint64_t>(reads.load() + 1));
                if (c)
                    reads.fetch_add(1, std::memory_order_relaxed);
                if (held)
                    reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    thr.emplace_back([&]() {
        for (int i = 0; i < 300; ++i) {
            if ((i % 2) == 0)
                mgr.ApplySnapshot({"127.0.0.1:19001", "127.0.0.1:19003"}, {"gl-0", "gl-2"});
            else
                mgr.ApplySnapshot({"127.0.0.1:19001", "127.0.0.1:19002"}, {"gl-0", "gl-1"});
            mgr.ApplySnapshot({}, {});  // must keep
            snaps.fetch_add(1, std::memory_order_relaxed);
        }
        stop.store(true, std::memory_order_relaxed);
    });

    for (auto &t : thr)
        t.join();

    if (!mgr.ready() || mgr.size() < 1) {
        std::printf("FAIL empty after race\n");
        return 1;
    }
    if (mgr.empty_snapshot_ignored() == 0) {
        std::printf("FAIL expected empty snapshot ignored\n");
        return 1;
    }
    if (!held) {
        std::printf("FAIL held channel lost unexpectedly\n");
        return 1;
    }
    std::printf("PASS channel_snapshot_race_test reads=%d snaps=%d ignored=%llu\n", reads.load(),
                snaps.load(), static_cast<unsigned long long>(mgr.empty_snapshot_ignored()));
    held.reset();
    mgr.Shutdown();
    return 0;
}
