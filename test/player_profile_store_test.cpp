/**
 * 注册事务创建 Profile；幂等重试不重复建号；老账号补档。
 */
#include "Connection.h"
#include "ConnectionPool.h"
#include "Logging.h"
#include "PlayerAccountStore.h"
#include "PlayerProfileStore.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
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
        std::printf("FAIL: MySQL pool not initialized\n");
        return 1;
    }
    Expect(PlayerAccountStore::Instance().EnsureTable(), "account table");
    Expect(PlayerProfileStore::Instance().EnsureTable(), "profile table");

    const std::string suffix = std::to_string(NowMs());
    const std::string device = "s1_prof_" + suffix;
    const std::string idem = "s1_idem_" + suffix;
    uint64_t pid = 0;
    std::string err;
    bool replayed = false;
    Expect(PlayerAccountStore::Instance().RegisterWithPasswordIdempotent(
               device, "HeroA", "hashhashhashhash", "saltsalt", 1000, idem, &pid, &err, &replayed),
           "register");
    Expect(pid != 0 && !replayed, "new account");
    PlayerProfileRow row;
    Expect(PlayerProfileStore::Instance().Load(pid, &row, &err) && row.exists, "profile exists");
    Expect(row.player_name == "HeroA", "name");
    Expect(row.max_hp == 100 && row.hp == 100 && row.max_mp == 100 && row.mp == 100, "hp/mp");
    Expect(row.move_speed >= 9.9f && row.move_speed <= 10.1f, "move_speed");
    Expect(row.stats_version == 1, "stats_version");

    uint64_t pid2 = 0;
    bool replayed2 = false;
    Expect(PlayerAccountStore::Instance().RegisterWithPasswordIdempotent(
               device, "HeroA", "hashhashhashhash", "saltsalt", 1000, idem, &pid2, &err, &replayed2),
           "retry");
    Expect(replayed2 && pid2 == pid, "idempotent same player");
    PlayerProfileRow row2;
    Expect(PlayerProfileStore::Instance().Load(pid2, &row2, &err) && row2.exists, "retry profile");
    Expect(row2.stats_version == row.stats_version, "no duplicate profile bump");

    // 老账号：删 Profile 后 EnsureDefault 补档
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    Expect(!!conn, "conn");
    if (conn) {
        conn->update("DELETE FROM player_profile WHERE player_id=" + std::to_string(pid));
    }
    PlayerProfileRow missing;
    Expect(PlayerProfileStore::Instance().Load(pid, &missing, &err) && !missing.exists, "deleted");
    Expect(PlayerProfileStore::Instance().EnsureDefault(pid, "HeroA", &err), "backfill");
    PlayerProfileRow back;
    Expect(PlayerProfileStore::Instance().Load(pid, &back, &err) && back.exists, "backfilled");
    Expect(back.player_name == "HeroA" && back.max_hp == 100, "backfill values");

    if (fails) {
        std::printf("player_profile_store_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK player_profile_store_test pid=%llu\n",
                static_cast<unsigned long long>(pid));
    return 0;
}
