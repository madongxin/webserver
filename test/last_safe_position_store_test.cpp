/**
 * S2：last_safe 落库拒绝非有限坐标；合法坐标可读写。
 */
#include "ConnectionPool.h"
#include "LastSafePositionStore.h"
#include "Logging.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        std::printf("FAIL: MySQL pool not initialized\n");
        return 1;
    }
    Expect(LastSafePositionStore::Instance().EnsureTable(), "ensure table");

    LastSafePositionRow row;
    row.player_id = 880000000ULL + static_cast<uint64_t>(
                                       std::chrono::steady_clock::now().time_since_epoch().count() %
                                       100000);
    row.realm_id = 1;
    row.map_template_id = 1001;
    row.x = std::numeric_limits<float>::quiet_NaN();
    row.y = 0;
    row.z = 0;
    uint64_t ver = 0;
    bool skipped = false;
    std::string err, code;
    Expect(!LastSafePositionStore::Instance().Save(row, 0, &ver, &skipped, &err, &code) && skipped,
           "nan rejected");

    row.x = -28.5f;
    row.y = -0.244f;
    row.z = -7.25f;
    row.yaw = 1.f;
    Expect(LastSafePositionStore::Instance().Save(row, 0, &ver, &skipped, &err, &code) && !skipped &&
               ver == 1,
           "first save");
    LastSafePositionRow loaded;
    Expect(LastSafePositionStore::Instance().Load(row.player_id, &loaded, &err) && loaded.exists,
           "load");
    Expect(std::fabs(loaded.x - row.x) < 0.01f, "x match");

    if (fails) {
        std::printf("last_safe_position_store_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK last_safe_position_store_test pid=%llu ver=%llu\n",
                static_cast<unsigned long long>(row.player_id),
                static_cast<unsigned long long>(ver));
    return 0;
}
