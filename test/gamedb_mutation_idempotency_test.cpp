/**
 * 阶段一最小闭环：ApplyMutation 幂等身份 + 转义/超长键
 */
#include "ConnectionPool.h"
#include "GameDbAssetStore.h"
#include "GameDbOutbox.h"
#include "Logging.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        std::printf("ERROR: MySQL required\n");
        return 1;
    }
    if (!GameDbAssetStore::Instance().EnsureTables() || !GameDbOutbox::Instance().EnsureTable())
        return Fail("tables");

    const uint64_t pid = 930000000ull + static_cast<uint64_t>(NowMs() % 1000000);
    const std::string key = "mut_idem_" + std::to_string(NowMs());

    std::string verr;
    if (GameDbAssetStore::ValidateIdempotencyKey(std::string(200, 'a'), &verr))
        return Fail("long key must fail validate");

    GameDbAssetStore::MutationResult bad;
    if (GameDbAssetStore::Instance().ApplyMutation(pid, std::string(200, 'x'), 0, "GRANT", 1001, 1,
                                                   &bad) ||
        bad.error_code != "INVALID_ARGUMENT")
        return Fail("long key apply");

    // 反斜杠 / 单引号不得破坏 SQL
    const std::string tricky = key + "_\\'_quote";
    GameDbAssetStore::MutationResult t0;
    if (!GameDbAssetStore::Instance().ApplyMutation(pid, tricky, 0, "GRANT", 1001, 2, &t0) || !t0.ok)
        return Fail("tricky key grant");
    GameDbAssetStore::MutationResult t1;
    if (!GameDbAssetStore::Instance().ApplyMutation(pid, tricky, 0, "GRANT", 1001, 2, &t1) || !t1.ok ||
        !t1.idempotent_hit || t1.asset_version != t0.asset_version || t1.remain_count != 2)
        return Fail("tricky key idempotent");

    const std::string key2 = key + "_conc";
    std::atomic<int> ok{0};
    std::atomic<uint64_t> ver{0};
    std::atomic<uint32_t> remain{0};
    // 并发度不超过连接池，避免占键事务 + 池耗尽假死
    constexpr int kConc = 4;
    std::vector<std::thread> th;
    for (int i = 0; i < kConc; ++i) {
        th.emplace_back([&]() {
            GameDbAssetStore::MutationResult r;
            if (GameDbAssetStore::Instance().ApplyMutation(pid, key2, 0, "GRANT", 2002, 1, &r) &&
                r.ok) {
                ok.fetch_add(1);
                ver.store(r.asset_version);
                remain.store(r.remain_count);
            }
        });
    }
    for (auto &t : th)
        t.join();
    if (ok.load() != kConc)
        return Fail("concurrent count");
    if (remain.load() != 1)
        return Fail("remain must be 1");

    // 同 key 不同 player
    GameDbAssetStore::MutationResult other;
    if (GameDbAssetStore::Instance().ApplyMutation(pid + 1, key2, 0, "GRANT", 2002, 1, &other))
        return Fail("other player should conflict");
    if (other.error_code != "IDEMPOTENCY_CONFLICT")
        return Fail("other player code");

    // 同 key 不同 count
    GameDbAssetStore::MutationResult diff;
    if (GameDbAssetStore::Instance().ApplyMutation(pid, key2, 0, "GRANT", 2002, 9, &diff))
        return Fail("diff payload should conflict");
    if (diff.error_code != "IDEMPOTENCY_CONFLICT")
        return Fail("diff payload code");

    GameDbAssetStore::OperationQuery q;
    if (!GameDbAssetStore::Instance().QueryOperationResult(pid, key2, "GRANT", &q) || !q.found ||
        q.status != "SUCCEEDED")
        return Fail("query status");

    std::printf("OK gamedb_mutation_idempotency_test ver=%llu remain=%u\n",
                (unsigned long long)ver.load(), remain.load());
    return 0;
}
