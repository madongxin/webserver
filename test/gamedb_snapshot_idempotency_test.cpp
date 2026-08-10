/**
 * 阶段一：SaveSnapshot 幂等 + 冲突
 */
#include "ConnectionPool.h"
#include "GameDbAssetStore.h"
#include "GameDbOutbox.h"
#include "Logging.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
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
        std::printf("ERROR: MySQL required for gamedb_snapshot_idempotency_test\n");
        return 1;
    }
    if (!GameDbAssetStore::Instance().EnsureTables() || !GameDbOutbox::Instance().EnsureTable())
        return Fail("tables");

    const uint64_t pid = 920000000ull + static_cast<uint64_t>(NowMs() % 1000000);
    const std::string key = "snap_idem_" + std::to_string(NowMs());
    std::map<uint32_t, uint32_t> bag{{1001, 3}, {1002, 1}};

    GameDbAssetStore::SnapshotResult empty;
    if (GameDbAssetStore::Instance().SaveSnapshot(pid, 0, bag, "", &empty))
        return Fail("empty key must fail");
    if (empty.error_code != "IDEMPOTENCY_REQUIRED")
        return Fail("empty key code");

    std::atomic<int> ok{0};
    std::atomic<uint64_t> ver{0};
    std::vector<std::thread> th;
    for (int i = 0; i < 10; ++i) {
        th.emplace_back([&]() {
            GameDbAssetStore::SnapshotResult r;
            if (GameDbAssetStore::Instance().SaveSnapshot(pid, 0, bag, key, &r) && r.ok) {
                ok.fetch_add(1);
                ver.store(r.asset_version);
            }
        });
    }
    for (auto &t : th)
        t.join();
    if (ok.load() != 10)
        return Fail("concurrent save count");
    const uint64_t v1 = ver.load();
    if (v1 == 0)
        return Fail("version");

    GameDbAssetStore::SnapshotResult again;
    if (!GameDbAssetStore::Instance().SaveSnapshot(pid, 0, bag, key, &again) || !again.ok ||
        !again.idempotent_hit || again.asset_version != v1)
        return Fail("idempotent replay");

    std::map<uint32_t, uint32_t> other{{1001, 99}};
    GameDbAssetStore::SnapshotResult conflict;
    if (GameDbAssetStore::Instance().SaveSnapshot(pid, 0, other, key, &conflict))
        return Fail("conflict should fail");
    if (conflict.error_code != "IDEMPOTENCY_CONFLICT")
        return Fail("conflict code");

    std::printf("OK gamedb_snapshot_idempotency_test ver=%llu\n",
                (unsigned long long)v1);
    std::fflush(stdout);
    std::_Exit(0);
}
