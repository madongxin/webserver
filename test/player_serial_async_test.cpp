/**
 * 阶段二：PlayerSerialQueue 异步 inflight — A 慢任务不阻塞同 shard B。
 */
#include "PlayerSerialQueue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

int main() {
    auto &q = PlayerSerialQueue::Instance();
    q.Stop();
    q.SetLimits(64, 256);
    q.Start(1);  // 单 shard：A 与 B 同 worker

    std::atomic<int> b_done{0};
    std::atomic<int> a_done{0};
    std::atomic<bool> a_started{false};
    std::mutex helper_mu;
    std::thread helper_a;
    std::thread helper_a2;

    const uint64_t a = 1;
    const uint64_t b = 2;  // same shard when shard_count=1

    if (!q.TryPost(a, [&]() {
            a_started.store(true);
            q.MarkAsyncInFlight(a);
            // 模拟慢下游：不阻塞 worker，稍后 Complete
            std::lock_guard<std::mutex> lk(helper_mu);
            helper_a = std::thread([&]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                q.CompleteAsyncInFlight(a, [&]() { a_done.fetch_add(1); });
            });
        })) {
        std::printf("FAIL post A\n");
        return 1;
    }

    // 等 A 进入 inflight
    for (int i = 0; i < 100 && !a_started.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const auto t0 = std::chrono::steady_clock::now();
    if (!q.TryPost(b, [&]() { b_done.fetch_add(1); })) {
        std::printf("FAIL post B\n");
        return 1;
    }

    for (int i = 0; i < 200 && b_done.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    if (b_done.load() != 1) {
        std::printf("FAIL B not done\n");
        return 1;
    }
    if (ms > 200) {
        std::printf("FAIL B blocked ms=%lld (A should not block shard)\n",
                    static_cast<long long>(ms));
        return 1;
    }

    for (int i = 0; i < 200 && a_done.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (a_done.load() != 1) {
        std::printf("FAIL A completion missing\n");
        return 1;
    }
    {
        std::lock_guard<std::mutex> lk(helper_mu);
        if (helper_a.joinable())
            helper_a.join();
    }

    // 同玩家有序：A inflight 期间再投递 A2，须在 Complete 之后
    std::atomic<int> order{0};
    std::atomic<int> a2{0};
    q.TryPost(a, [&]() {
        q.MarkAsyncInFlight(a);
        std::lock_guard<std::mutex> lk(helper_mu);
        helper_a2 = std::thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            q.CompleteAsyncInFlight(a, [&]() { order.store(1); });
        });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    q.TryPost(a, [&]() {
        if (order.load() != 1) {
            std::printf("FAIL A2 ran before A1 complete\n");
            std::_Exit(1);
        }
        a2.store(1);
    });
    for (int i = 0; i < 100 && a2.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (a2.load() != 1) {
        std::printf("FAIL A2 missing\n");
        return 1;
    }
    {
        std::lock_guard<std::mutex> lk(helper_mu);
        if (helper_a2.joinable())
            helper_a2.join();
    }

    // completion 在外部队列已满时仍必须回投（不得跨线程改状态）
    q.SetLimits(2, 2);
    std::atomic<int> on_serial{0};
    std::atomic<bool> saw_worker{false};
    std::thread::id worker_id;
    q.TryPost(a, [&]() {
        worker_id = std::this_thread::get_id();
        saw_worker.store(true);
        q.MarkAsyncInFlight(a);
    });
    for (int i = 0; i < 100 && !saw_worker.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // 填满外部队列
    q.TryPost(b, []() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
    q.TryPost(b, []() {});
    std::atomic<bool> completed{false};
    std::thread completer([&]() {
        const bool ok = q.CompleteAsyncInFlight(a, [&]() {
            if (std::this_thread::get_id() == worker_id)
                on_serial.fetch_add(1);
            completed.store(true);
        });
        if (!ok)
            std::_Exit(2);
    });
    completer.join();
    for (int i = 0; i < 200 && !completed.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!completed.load() || on_serial.load() != 1) {
        std::printf("FAIL completion under overload on_serial=%d\n", on_serial.load());
        return 1;
    }

    // 链式异步：completion 内再次 MarkAsyncInFlight 时，同玩家其它任务不得并发
    {
        std::atomic<int> concurrent{0};
        std::atomic<int> max_concurrent{0};
        std::atomic<bool> chain_done{false};
        std::atomic<bool> other_ran{false};
        std::atomic<bool> other_before_chain{false};
        std::thread helper_chain;
        std::thread helper_chain2;

        q.SetLimits(64, 256);
        if (!q.TryPost(a, [&]() {
                q.MarkAsyncInFlight(a);
                helper_chain = std::thread([&]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    q.CompleteAsyncInFlight(a, [&]() {
                        const int c = concurrent.fetch_add(1) + 1;
                        int prev = max_concurrent.load();
                        while (c > prev && !max_concurrent.compare_exchange_weak(prev, c)) {
                        }
                        q.MarkAsyncInFlight(a);  // 链式第二段
                        concurrent.fetch_sub(1);
                        helper_chain2 = std::thread([&]() {
                            std::this_thread::sleep_for(std::chrono::milliseconds(80));
                            q.CompleteAsyncInFlight(a, [&]() {
                                const int c2 = concurrent.fetch_add(1) + 1;
                                int prev2 = max_concurrent.load();
                                while (c2 > prev2 &&
                                       !max_concurrent.compare_exchange_weak(prev2, c2)) {
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                concurrent.fetch_sub(1);
                                chain_done.store(true);
                            });
                        });
                    });
                });
            })) {
            std::printf("FAIL post chain A\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!q.TryPost(a, [&]() {
                if (!chain_done.load())
                    other_before_chain.store(true);
                const int c = concurrent.fetch_add(1) + 1;
                int prev = max_concurrent.load();
                while (c > prev && !max_concurrent.compare_exchange_weak(prev, c)) {
                }
                if (c > 1) {
                    std::printf("FAIL chain_serialization_violated=%d\n", c);
                    std::_Exit(1);
                }
                concurrent.fetch_sub(1);
                other_ran.store(true);
            })) {
            std::printf("FAIL post deferred A\n");
            return 1;
        }
        for (int i = 0; i < 200 && (!chain_done.load() || !other_ran.load()); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (helper_chain.joinable())
            helper_chain.join();
        if (helper_chain2.joinable())
            helper_chain2.join();
        if (!chain_done.load() || !other_ran.load()) {
            std::printf("FAIL chain incomplete chain=%d other=%d\n", chain_done.load(),
                        other_ran.load());
            return 1;
        }
        if (other_before_chain.load()) {
            std::printf("FAIL chain_serialization_violated deferred ran early\n");
            return 1;
        }
        if (max_concurrent.load() > 1) {
            std::printf("FAIL chain_serialization_violated max_concurrent=%d\n",
                        max_concurrent.load());
            return 1;
        }
    }

    q.DrainForTest();

    // DRAINING：拒绝新任务，已开始的 async completion 仍在 worker 完成
    {
        q.SetLimits(64, 256);
        std::atomic<int> drained_done{0};
        std::atomic<bool> marked{false};
        std::thread::id wid;
        q.TryPost(a, [&]() {
            wid = std::this_thread::get_id();
            q.MarkAsyncInFlight(a);
            marked.store(true);
        });
        for (int i = 0; i < 100 && !marked.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        q.BeginDrain(std::chrono::milliseconds(2000));
        if (q.TryPost(b, []() {})) {
            std::printf("FAIL drain still accepts TryPost\n");
            return 1;
        }
        std::atomic<bool> on_worker{false};
        std::thread late([&]() {
            q.CompleteAsyncInFlight(a, [&]() {
                on_worker.store(std::this_thread::get_id() == wid);
                drained_done.fetch_add(1);
            });
        });
        late.join();
        for (int i = 0; i < 200 && drained_done.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (drained_done.load() != 1 || !on_worker.load()) {
            std::printf("FAIL drain completion\n");
            return 1;
        }
    }

    // STOPPED 后迟到 callback：不执行 completion（调用方负责取消）
    {
        q.Stop();
        std::atomic<int> applied{0};
        const bool enq = q.CompleteAsyncInFlight(a, [&]() { applied.fetch_add(1); });
        if (enq || applied.load() != 0) {
            std::printf("FAIL stopped inline completion\n");
            return 1;
        }
        if (q.TryPost(a, [&]() { applied.fetch_add(1); })) {
            std::printf("FAIL stopped TryPost\n");
            return 1;
        }
    }

    // 重新 Start 验证仍可用
    q.Start(1);
    std::atomic<int> once{0};
    q.TryPost(a, [&]() { once.fetch_add(1); });
    q.DrainForTest();
    if (once.load() != 1) {
        std::printf("FAIL restart\n");
        return 1;
    }

    q.Stop();
    std::printf("PASS player_serial_async_test b_latency_ms=%lld\n", static_cast<long long>(ms));
    return 0;
}
