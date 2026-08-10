/**
 * 阶段一：队列过载 TryPost 拒绝（Gateway 关键路径同语义）
 */
#include "PlayerSerialQueue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

}  // namespace

int main() {
    PlayerSerialQueue::Instance().SetLimits(1, 1);
    PlayerSerialQueue::Instance().Start(1);
    std::atomic<int> ran{0};
    // 占住 worker
    if (!PlayerSerialQueue::Instance().TryPost(42, [&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            ran.fetch_add(1);
        }))
        return Fail("first post");
    int rejected = 0;
    for (int i = 0; i < 20; ++i) {
        if (!PlayerSerialQueue::Instance().TryPost(42, []() {}))
            ++rejected;
    }
    if (rejected < 1)
        return Fail("expect overload reject");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    PlayerSerialQueue::Instance().Stop();
    if (ran.load() < 1)
        return Fail("worker ran");
    std::printf("OK gateway_overload_test rejected=%d\n", rejected);
    return 0;
}
