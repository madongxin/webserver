/**
 * 断线异步化：
 * 1) MarkDisconnectedAsync / UnbindPlayerAsync 不得阻塞调用线程
 * 2) 本地 Redis fallback（GatewayDisconnectAsync）不得阻塞投递侧
 */
#include "GatewayAuthClients.h"
#include "GatewayDisconnectAsync.h"
#include "Logging.h"
#include "SessionRpcClient.h"
#include "gamelogic_rpc.pb.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

int main() {
    Logger::setLogLevel(Logger::WARN);

    const int timeout_ms = 3000;
    if (!SessionRpcClient::Instance().Init("127.0.0.1:1", timeout_ms)) {
        std::printf("FAIL SessionRpcClient Init\n");
        return 1;
    }
    if (!GatewayAuthClients::Instance().InitAuthSession("127.0.0.1:1", timeout_ms) ||
        !GatewayAuthClients::Instance().InitLogicChannels({"127.0.0.1:1"}, {"gl-dead"},
                                                          timeout_ms)) {
        std::printf("FAIL GatewayAuthClients Init\n");
        return 1;
    }

    {
        const auto t0 = std::chrono::steady_clock::now();
        constexpr int kN = 64;
        for (int i = 0; i < kN; ++i) {
            SessionRpcClient::Instance().MarkDisconnectedAsync(1000 + static_cast<uint64_t>(i),
                                                               "tok", 1);
            glrpc::UnbindPlayerRequest ureq;
            ureq.set_player_id(1000 + static_cast<uint64_t>(i));
            ureq.set_session_id("s");
            ureq.set_fence_token("tok");
            ureq.set_reason("storm_test");
            GatewayAuthClients::Instance().UnbindPlayerAsync("gl-dead", ureq);
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (ms > 500) {
            std::printf("FAIL disconnect storm blocked ms=%lld (limit 500)\n",
                        static_cast<long long>(ms));
            return 1;
        }
        std::printf("OK brpc disconnect storm ms=%lld\n", static_cast<long long>(ms));
    }

    // 本地 Redis fallback：注入阻塞 executor，投递必须仍很快
    {
        GatewayDisconnectAsync::Instance().Start(4096);
        std::atomic<int> ran{0};
        GatewayDisconnectAsync::Instance().SetExecutorForTest([&](const GatewayDisconnectAsync::Task &) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ran.fetch_add(1);
        });
        const auto t0 = std::chrono::steady_clock::now();
        constexpr int kN = 64;
        int enq_ok = 0;
        for (int i = 0; i < kN; ++i) {
            if (GatewayDisconnectAsync::Instance().EnqueueMarkDisconnected(
                    2000 + static_cast<uint64_t>(i), "tok", 1))
                ++enq_ok;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (enq_ok != kN) {
            std::printf("FAIL redis-fallback enqueue ok=%d want=%d\n", enq_ok, kN);
            GatewayDisconnectAsync::Instance().Stop();
            return 1;
        }
        if (ms > 100) {
            std::printf("FAIL redis-fallback enqueue blocked ms=%lld (limit 100)\n",
                        static_cast<long long>(ms));
            GatewayDisconnectAsync::Instance().Stop();
            return 1;
        }
        // 等待后台收敛
        for (int i = 0; i < 200 && ran.load() < kN; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (ran.load() != kN) {
            std::printf("FAIL redis-fallback executed=%d want=%d\n", ran.load(), kN);
            GatewayDisconnectAsync::Instance().Stop();
            return 1;
        }
        GatewayDisconnectAsync::Instance().Stop();
        std::printf("OK redis-fallback disconnect storm enqueue_ms=%lld executed=%d\n",
                    static_cast<long long>(ms), ran.load());
    }

    // 有界 Stop：1.5s 阻塞 executor 下 Stop(50ms) 必须很快返回，且可再次 Start
    {
        GatewayDisconnectAsync::Instance().Start(64);
        auto still_running = std::make_shared<std::atomic<bool>>(true);
        GatewayDisconnectAsync::Instance().SetExecutorForTest(
            [still_running](const GatewayDisconnectAsync::Task &) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                still_running->store(false, std::memory_order_release);
            });
        if (!GatewayDisconnectAsync::Instance().EnqueueMarkDisconnected(3001, "tok", 1)) {
            std::printf("FAIL bounded-stop enqueue\n");
            GatewayDisconnectAsync::Instance().Stop();
            return 1;
        }
        for (int i = 0; i < 200 && GatewayDisconnectAsync::Instance().pending() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto t0 = std::chrono::steady_clock::now();
        GatewayDisconnectAsync::Instance().Stop(std::chrono::milliseconds(50));
        const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        if (stop_ms > 200) {
            std::printf("FAIL bounded-stop elapsed_ms=%lld (limit 200, requested 50)\n",
                        static_cast<long long>(stop_ms));
            return 1;
        }
        if (!still_running->load(std::memory_order_acquire)) {
            std::printf("FAIL bounded-stop finished executor before return\n");
            return 1;
        }
        GatewayDisconnectAsync::Instance().Start(8);
        std::atomic<int> ran2{0};
        GatewayDisconnectAsync::Instance().SetExecutorForTest(
            [&](const GatewayDisconnectAsync::Task &) { ran2.fetch_add(1); });
        if (!GatewayDisconnectAsync::Instance().EnqueueMarkDisconnected(3002, "tok", 1)) {
            std::printf("FAIL restart enqueue\n");
            GatewayDisconnectAsync::Instance().Stop();
            return 1;
        }
        for (int i = 0; i < 100 && ran2.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (ran2.load() != 1) {
            std::printf("FAIL restart execute\n");
            GatewayDisconnectAsync::Instance().Stop();
            return 1;
        }
        GatewayDisconnectAsync::Instance().Stop();
        std::printf("OK bounded-stop elapsed_ms=%lld\n", static_cast<long long>(stop_ms));
    }

    // 队列满：投递立即返回
    {
        GatewayDisconnectAsync::Instance().Start(4);
        GatewayDisconnectAsync::Instance().SetExecutorForTest(
            [](const GatewayDisconnectAsync::Task &) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            });
        int ok = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 16; ++i) {
            if (GatewayDisconnectAsync::Instance().EnqueueMarkDisconnected(
                    4000 + static_cast<uint64_t>(i), "tok", 1))
                ++ok;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (ms > 50) {
            std::printf("FAIL queue-full enqueue blocked ms=%lld\n",
                        static_cast<long long>(ms));
            GatewayDisconnectAsync::Instance().Stop();
            return 1;
        }
        if (ok > 4) {
            std::printf("FAIL queue-full accepted=%d\n", ok);
            GatewayDisconnectAsync::Instance().Stop();
            return 1;
        }
        if (GatewayDisconnectAsync::Instance().dropped() == 0) {
            std::printf("FAIL queue-full dropped=0\n");
            GatewayDisconnectAsync::Instance().Stop();
            return 1;
        }
        GatewayDisconnectAsync::Instance().Stop(std::chrono::milliseconds(50));
        std::printf("OK queue-full enqueue_ms=%lld accepted=%d dropped=%llu\n",
                    static_cast<long long>(ms), ok,
                    static_cast<unsigned long long>(GatewayDisconnectAsync::Instance().dropped()));
    }

    std::printf("PASS gateway_disconnect_async_test\n");
    return 0;
}
