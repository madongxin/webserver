/**
 * 阶段一：未知结果查询闭环 QueryOperationResult
 */
#include "ConnectionPool.h"
#include "GameDbAssetStore.h"
#include "Logging.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>

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
        std::printf("ERROR: MySQL required for gamedb_unknown_result_test\n");
        return 1;
    }
    if (!GameDbAssetStore::Instance().EnsureTables())
        return Fail("tables");

    const uint64_t pid = 930000000ull + static_cast<uint64_t>(NowMs() % 1000000);
    const std::string key = "unk_q_" + std::to_string(NowMs());

    GameDbAssetStore::OperationQuery miss;
    if (!GameDbAssetStore::Instance().QueryOperationResult(pid, key, "SAVE_SNAPSHOT", &miss))
        return Fail("query miss call");
    if (miss.found)
        return Fail("should not found");

    std::map<uint32_t, uint32_t> bag{{7, 2}};
    GameDbAssetStore::SnapshotResult sr;
    if (!GameDbAssetStore::Instance().SaveSnapshot(pid, 0, bag, key, &sr) || !sr.ok)
        return Fail("save");

    GameDbAssetStore::OperationQuery hit;
    if (!GameDbAssetStore::Instance().QueryOperationResult(pid, key, "SAVE_SNAPSHOT", &hit))
        return Fail("query hit");
    if (!hit.found || !hit.completed_ok || hit.asset_version != sr.asset_version)
        return Fail("query result mismatch");

    GameDbAssetStore::SnapshotResult again;
    if (!GameDbAssetStore::Instance().SaveSnapshot(pid, 0, bag, key, &again) || !again.ok ||
        !again.idempotent_hit || again.asset_version != hit.asset_version)
        return Fail("retry after unknown");

    std::printf("OK gamedb_unknown_result_test\n");
    std::fflush(stdout);
    std::_Exit(0);
}
