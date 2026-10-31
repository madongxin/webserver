/**
 * GatewayConnRegistry 单锁基线。读写比 9:1。
 * 不改注册表实现；只打印吞吐和 P99，供 docs/perf_audit 记录。
 */
#include "GatewayConnRegistry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

GatewayConnRegistry::Bind MakeBind(uint64_t id) {
    GatewayConnRegistry::Bind b;
    b.player_id = id;
    b.session_id = "s-" + std::to_string(id);
    b.token = "fence";
    b.generation = 1;
    b.gamelogic_instance_id = "gl-0";
    b.gateway_instance_id = "gw-0";
    b.map_instance_id = 1002;
    b.map_owner_epoch = 1;
    b.route_version = 1;
    return b;
}

struct Result {
    uint64_t ops = 0;
    std::vector<uint32_t> us;
};

Result Run(int connections, int threads, int seconds) {
    auto &reg = GatewayConnRegistry::Instance();
    for (int i = 1; i <= connections; ++i)
        reg.Remember(static_cast<uint64_t>(i), MakeBind(static_cast<uint64_t>(i)));

    std::atomic<int> go{0};
    std::vector<Result> parts(static_cast<size_t>(threads));
    std::vector<std::thread> pool;
    const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t]() {
            GatewayConnRegistry::Bind out;
            uint64_t rng = 0x9e3779b97f4a7c15ull * static_cast<uint64_t>(t + 1);
            go.fetch_add(1);
            while (go.load() < threads)
                std::this_thread::yield();
            auto &part = parts[static_cast<size_t>(t)];
            part.us.reserve(200000);
            while (std::chrono::steady_clock::now() < stop) {
                rng = rng * 6364136223846793005ull + 1;
                const uint64_t id = (rng >> 33) % static_cast<uint64_t>(connections) + 1;
                const auto t0 = std::chrono::steady_clock::now();
                if ((rng & 1023u) < 922u) {
                    if ((rng & 1u) == 0)
                        reg.FindByPlayer(id, &out);
                    else
                        reg.FindBySession("s-" + std::to_string(id), &out);
                } else {
                    reg.ApplyRoute(id, "gl-0", 1002, 1, ~uint64_t{0});
                }
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
                part.us.push_back(us < 0 ? 0 : static_cast<uint32_t>(us));
                ++part.ops;
            }
        });
    }
    Result all;
    for (size_t i = 0; i < pool.size(); ++i) {
        pool[i].join();
        all.ops += parts[i].ops;
        all.us.insert(all.us.end(), parts[i].us.begin(), parts[i].us.end());
    }
    for (int i = 1; i <= connections; ++i)
        reg.Forget(static_cast<uint64_t>(i));
    return all;
}

uint32_t Percentile(std::vector<uint32_t> *v, double p) {
    if (v->empty())
        return 0;
    std::sort(v->begin(), v->end());
    size_t i = static_cast<size_t>(p * static_cast<double>(v->size() - 1));
    return (*v)[i];
}

}  // namespace

int main() {
    const int threads = 4;
    const int seconds = 2;
    std::printf("threads=%d seconds=%d ratio=read9_write1 build=Debug\n", threads, seconds);
    for (int n : {1000, 10000}) {
        auto r = Run(n, threads, seconds);
        const uint32_t p50 = Percentile(&r.us, 0.50);
        const uint32_t p99 = Percentile(&r.us, 0.99);
        const double qps = static_cast<double>(r.ops) / static_cast<double>(seconds);
        std::printf("n=%d ops=%llu qps=%.0f p50_us=%u p99_us=%u\n", n,
                    static_cast<unsigned long long>(r.ops), qps, p50, p99);
    }
    return 0;
}
