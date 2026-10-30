/**
 * RedisPool::Acquire 不得在持有池锁时做 Ping/Connect。
 * 一个连接的 Connect 阻塞时，其它线程仍应能立刻借到健康连接。
 */
#include "Logging.h"
#include "RedisPool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

class FakeRedisClient : public RedisClient {
public:
    explicit FakeRedisClient(bool slow_connect) : slow_connect_(slow_connect), connected_(true) {}

    bool IsConnected() const override { return connected_; }

    bool Ping() override { return connected_ && !slow_connect_; }

    bool Connect(const std::string &, int, const std::string &) override {
        if (slow_connect_)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        connected_ = true;
        slow_connect_ = false;
        return true;
    }

private:
    bool slow_connect_;
    bool connected_;
};

bool Install(int fast_n, int slow_n) {
    RedisPool::Instance().Shutdown();
    std::vector<std::unique_ptr<RedisClient>> clients;
    for (int i = 0; i < fast_n; ++i)
        clients.push_back(std::unique_ptr<RedisClient>(new FakeRedisClient(false)));
    for (int i = 0; i < slow_n; ++i)
        clients.push_back(std::unique_ptr<RedisClient>(new FakeRedisClient(true)));
    return RedisPool::Instance().InitWithClientsForTest(std::move(clients));
}

int TestConnectDoesNotSerialize() {
    if (!Install(7, 1)) {
        std::printf("FAIL install\n");
        return 1;
    }
    constexpr int kThreads = 8;
    std::atomic<int> ready{0};
    std::atomic<int> fast{0};
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            ready.fetch_add(1);
            while (ready.load() < kThreads)
                std::this_thread::yield();
            const auto t0 = std::chrono::steady_clock::now();
            auto lease = RedisPool::Instance().Acquire(2000);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            if (lease)
                ok.fetch_add(1);
            if (ms < 300)
                fast.fetch_add(1);
            lease = RedisPool::Lease();
        });
    }
    for (auto &th : threads)
        th.join();
    RedisPool::Instance().Shutdown();
    if (ok.load() != kThreads) {
        std::printf("FAIL ok=%d\n", ok.load());
        return 1;
    }
    if (fast.load() < 7) {
        std::printf("FAIL fast_acquires=%d (Connect held the pool lock)\n", fast.load());
        return 1;
    }
    std::printf("connect_not_under_lock fast=%d\n", fast.load());
    return 0;
}

int TestShutdownDuringConnect() {
    if (!Install(0, 1)) {
        std::printf("FAIL install slow\n");
        return 1;
    }
    std::atomic<bool> started{false};
    std::thread acq([&]() {
        started.store(true);
        auto lease = RedisPool::Instance().Acquire(3000);
        lease = RedisPool::Lease();
    });
    while (!started.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t0 = std::chrono::steady_clock::now();
    RedisPool::Instance().Shutdown(std::chrono::milliseconds(3000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    acq.join();
    if (ms > 2500) {
        std::printf("FAIL shutdown waited out the deadline ms=%ld\n", static_cast<long>(ms));
        return 1;
    }
    auto denied = RedisPool::Instance().Acquire(50);
    if (denied) {
        std::printf("FAIL acquire after shutdown\n");
        return 1;
    }
    std::printf("shutdown_during_connect ms=%ld\n", static_cast<long>(ms));
    return 0;
}

int TestOldGenerationDoesNotReturn() {
    if (!Install(1, 0)) {
        std::printf("FAIL install gen\n");
        return 1;
    }
    const uint64_t gen1 = RedisPool::Instance().generation();
    auto old = RedisPool::Instance().Acquire(1000);
    if (!old) {
        std::printf("FAIL old acquire\n");
        return 1;
    }
    std::thread stopper([]() {
        RedisPool::Instance().Shutdown(std::chrono::milliseconds(2000));
    });
    const auto wait_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (RedisPool::Instance().ready() && std::chrono::steady_clock::now() < wait_until)
        std::this_thread::yield();
    std::vector<std::unique_ptr<RedisClient>> fresh;
    fresh.push_back(std::unique_ptr<RedisClient>(new FakeRedisClient(false)));
    fresh.push_back(std::unique_ptr<RedisClient>(new FakeRedisClient(false)));
    if (!RedisPool::Instance().InitWithClientsForTest(std::move(fresh))) {
        std::printf("FAIL reinstall\n");
        old = RedisPool::Lease();
        stopper.join();
        return 1;
    }
    const uint64_t gen2 = RedisPool::Instance().generation();
    old = RedisPool::Lease();
    stopper.join();
    if (!(gen2 > gen1)) {
        std::printf("FAIL generation gen1=%llu gen2=%llu\n",
                    static_cast<unsigned long long>(gen1),
                    static_cast<unsigned long long>(gen2));
        return 1;
    }
    auto a = RedisPool::Instance().Acquire(500);
    auto b = RedisPool::Instance().Acquire(500);
    if (!a || !b) {
        std::printf("FAIL new pool exhausted by old lease\n");
        return 1;
    }
    a = RedisPool::Lease();
    b = RedisPool::Lease();
    RedisPool::Instance().Shutdown();
    std::printf("generation_isolated %llu -> %llu\n",
                static_cast<unsigned long long>(gen1),
                static_cast<unsigned long long>(gen2));
    return 0;
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::ERROR);
    RedisPool::Instance().Shutdown();
    if (int rc = TestConnectDoesNotSerialize())
        return rc;
    if (int rc = TestShutdownDuringConnect())
        return rc;
    if (int rc = TestOldGenerationDoesNotReturn())
        return rc;
    std::printf("redis_pool_acquire_test PASS\n");
    return 0;
}
