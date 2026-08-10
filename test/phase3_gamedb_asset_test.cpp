/**
 * 阶段三：GameDB 资产版本 CAS + 幂等 + Outbox 双 publisher Claim
 */
#include "ConnectionPool.h"
#include "GameDbAssetStore.h"
#include "GameDbOutbox.h"
#include "Logging.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++fails;
    }
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        std::printf("SKIP: MySQL pool not initialized\n");
        return 0;
    }
    Expect(GameDbAssetStore::Instance().EnsureTables(), "asset tables");
    Expect(GameDbOutbox::Instance().EnsureTable(), "outbox table");

    const uint64_t pid = 910000000ull + static_cast<uint64_t>(NowMs() % 1000000);
    const std::string idem = "p3_asset_" + std::to_string(NowMs());

    GameDbAssetStore::MutationResult r1;
    Expect(GameDbAssetStore::Instance().ApplyMutation(pid, idem, 0, "GRANT", 1001, 5, &r1),
           "grant ok");
    Expect(r1.ok && r1.remain_count == 5 && r1.asset_version >= 2, "grant result");

    GameDbAssetStore::MutationResult r2;
    Expect(GameDbAssetStore::Instance().ApplyMutation(pid, idem, 0, "GRANT", 1001, 5, &r2),
           "idempotent retry");
    Expect(r2.ok && r2.idempotent_hit && r2.remain_count == 5 &&
               r2.asset_version == r1.asset_version,
           "idempotent same result");

    GameDbAssetStore::MutationResult r3;
    Expect(!GameDbAssetStore::Instance().ApplyMutation(pid, idem + "_bad", r1.asset_version + 9,
                                                       "GRANT", 1001, 1, &r3) ||
               !r3.ok,
           "version conflict");
    Expect(r3.error_code == "VERSION_CONFLICT", "conflict code");

    std::map<uint32_t, uint32_t> bag;
    uint64_t ver = 0;
    Expect(GameDbAssetStore::Instance().LoadInventory(pid, &bag, &ver), "load inv");
    Expect(bag[1001] == 5 && ver == r1.asset_version, "bag matches");

    // 双 publisher：两线程 ClaimUnpublished 不得拿到同一行
    const std::string idem2 = "p3_outbox_" + std::to_string(NowMs());
    GameDbAssetStore::MutationResult rg;
    Expect(GameDbAssetStore::Instance().ApplyMutation(pid, idem2, ver, "GRANT", 1002, 2, &rg),
           "second grant for outbox");

    std::atomic<int> claimed{0};
    std::atomic<int> overlap{0};
    std::mutex seen_mu;
    std::vector<uint64_t> seen;
    auto worker = [&]() {
        for (int i = 0; i < 20; ++i) {
            std::vector<GameDbOutboxRow> rows;
            if (!GameDbOutbox::Instance().ClaimUnpublished(10, &rows))
                continue;
            for (const auto &row : rows) {
                claimed.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lk(seen_mu);
                    for (uint64_t id : seen) {
                        if (id == row.id)
                            overlap.fetch_add(1);
                    }
                    seen.push_back(row.id);
                }
                using namespace std::chrono;
                GameDbOutbox::Instance().MarkPublished(
                    row.id,
                    duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    std::thread t1(worker);
    std::thread t2(worker);
    t1.join();
    t2.join();
    Expect(overlap.load() == 0, "dual publisher no overlap claims");
    Expect(claimed.load() >= 1, "claimed at least one outbox row");

    if (fails) {
        std::printf("phase3_gamedb_asset_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK phase3_gamedb_asset_test\n");
    std::fflush(stdout);
    std::_Exit(0);
}
