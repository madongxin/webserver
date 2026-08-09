/**
 * Channel shared_ptr：ApplySnapshot 热更新后在途持有不 UAF
 */
#include "BrpcChannelManager.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

int main() {
    auto &mgr = BrpcChannelManager::Instance();
    mgr.Shutdown();
    if (!mgr.Init({"127.0.0.1:1"}, {"gl-hold"}, 100)) {
        std::printf("FAIL channel init\n");
        return 1;
    }
    auto held = mgr.SharedChannelForInstance("gl-hold");
    if (!held) {
        std::printf("FAIL shared channel null\n");
        return 1;
    }
    brpc::Channel *raw = held.get();
    std::atomic<int> hits{0};
    std::vector<std::thread> thr;
    thr.emplace_back([&]() {
        for (int i = 0; i < 200; ++i) {
            auto c = mgr.SharedChannelForPlayer(i);
            if (c)
                hits.fetch_add(1);
            if (i == 50)
                mgr.ApplySnapshot({"127.0.0.1:2"}, {"gl-new"});
        }
    });
    thr.emplace_back([&]() {
        for (int i = 0; i < 100; ++i) {
            if (held)
                hits.fetch_add(1);
        }
    });
    for (auto &t : thr)
        t.join();
    if (held.get() != raw) {
        std::printf("FAIL held pointer mutated\n");
        return 1;
    }
    if (mgr.SharedChannelForInstance("gl-hold") != nullptr) {
        std::printf("FAIL old id still in map\n");
        return 1;
    }
    held.reset();
    mgr.Shutdown();
    std::printf("PASS phase1_channel_hold_test hits=%d\n", hits.load());
    return 0;
}
