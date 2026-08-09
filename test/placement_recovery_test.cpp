/**
 * 阶段二：PlacementRecoveryScheduler — 过期 lease → RECOVERING → Migrate
 */
#include "PlacementRecoveryScheduler.h"
#include "PlacementStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

namespace {

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool InitRedis() {
    std::ifstream in(RedisConfigPath::RedisCnf());
    if (!in)
        return false;
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key == "ip")
            host = val;
        else if (key == "port")
            port = std::atoi(val.c_str());
        else if (key == "password")
            password = val;
    }
    return RedisPool::Instance().Init(host, port, password, 4);
}

int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

}  // namespace

int main() {
    if (!InitRedis()) {
        std::printf("SKIP placement_recovery_test (Redis unavailable)\n");
        return 0;
    }
    const std::string prefix = "gamemesh:dev:placerecovertest:";
    if (!PlacementStore::Instance().InitFromSessionPrefix(prefix, 2))
        return Fail("init");
    PlacementStore::Instance().SetLogicOwners({"gl-0", "gl-1"});

    ResolveOrCreateInput in;
    in.realm_id = 1;
    in.map_template_id = 9001;
    in.force_new = true;
    in.preferred_owner = "gl-0";
    ResolveOrCreateResult out;
    if (!PlacementStore::Instance().ResolveOrCreate(in, &out) || !out.ok)
        return Fail("create");
    const uint64_t mid = out.placement.map_instance_id;
    const uint64_t epoch = out.placement.owner_epoch;
    // 等 lease 过期（Init lease=2s）
    std::this_thread::sleep_for(std::chrono::seconds(3));

    PlacementRecoveryScheduler::Instance().SetScanCount(64);
    for (int i = 0; i < 5; ++i)
        PlacementRecoveryScheduler::Instance().Tick();

    PlacementRecord rec;
    if (!PlacementStore::Instance().Get(mid, &rec))
        return Fail("get after recover");
    if (rec.state != PlacementState::Ready)
        return Fail("not READY after auto migrate");
    if (rec.owner_logic_server_id != "gl-1")
        return Fail("owner not switched to gl-1");
    if (rec.owner_epoch <= epoch)
        return Fail("epoch not bumped");
    if (PlacementRecoveryScheduler::Instance().recover_ok() == 0)
        return Fail("recover_ok counter");

    std::printf("OK placement_recovery_test map=%llu %llu->%llu owner=%s\n",
                (unsigned long long)mid, (unsigned long long)epoch,
                (unsigned long long)rec.owner_epoch, rec.owner_logic_server_id.c_str());
    std::printf("PASS placement_recovery_test\n");
    return 0;
}
