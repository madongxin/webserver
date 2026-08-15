/**
 * S2：公共池 50/51 分配；100 并发占位无实例超过容量；指定实例满员不换图；幂等。
 */
#include "PlacementStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

int Fail(const char *msg) {
    std::printf("FAIL %s\n", msg);
    return 1;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

bool InitRedis() {
    const std::string &path = RedisConfigPath::RedisCnf();
    std::ifstream in(path);
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
    return RedisPool::Instance().Init(host, port, password, 8);
}

}  // namespace

int main() {
    if (!InitRedis())
        return Fail("redis unavailable");
    const std::string prefix = "gamemesh:test:map_occ:" +
                               std::to_string(static_cast<unsigned long long>(::getpid())) + ":";
    if (!PlacementStore::Instance().InitFromSessionPrefix(prefix))
        return Fail("PlacementStore init");
    PlacementStore::Instance().SetLogicOwners({"gl-0", "gl-1"});
    PlacementStore::Instance().SetPublicMapCapacity(50);

    const uint32_t realm = 1;
    const uint64_t tpl = 1001;

    // 顺序 50/51
    uint64_t first = 0;
    for (int i = 1; i <= 50; ++i) {
        ResolveOrCreateInput in;
        in.realm_id = realm;
        in.map_template_id = tpl;
        in.player_id = 1000 + static_cast<uint64_t>(i);
        in.operation_id = "seq-" + std::to_string(i);
        in.capacity = 50;
        ResolveOrCreateResult out;
        if (!PlacementStore::Instance().ReservePublicSlot(in, &out) || !out.ok)
            return Fail("reserve 1-50");
        if (first == 0)
            first = out.placement.map_instance_id;
        else if (out.placement.map_instance_id != first)
            return Fail("first 50 not same instance");
    }
    if (PlacementStore::Instance().Occupancy(first) != 50)
        return Fail("occupancy != 50");

    ResolveOrCreateInput in51;
    in51.realm_id = realm;
    in51.map_template_id = tpl;
    in51.player_id = 2051;
    in51.operation_id = "seq-51";
    in51.capacity = 50;
    ResolveOrCreateResult out51;
    if (!PlacementStore::Instance().ReservePublicSlot(in51, &out51) || !out51.ok)
        return Fail("51st should create new instance");
    if (out51.placement.map_instance_id == first)
        return Fail("51st stayed on full instance");
    if (PlacementStore::Instance().Occupancy(first) != 50)
        return Fail("first still 50 after overflow");

    // 指定满员实例：错误，不换图
    ResolveOrCreateInput pin;
    pin.realm_id = realm;
    pin.map_template_id = tpl;
    pin.map_instance_id = first;
    pin.player_id = 2099;
    pin.operation_id = "pin-full";
    pin.capacity = 50;
    ResolveOrCreateResult pout;
    if (PlacementStore::Instance().ReservePublicSlot(pin, &pout) && pout.ok)
        return Fail("pinned full should fail");
    if (pout.error_code != "ERR_MAP_FULL")
        return Fail("expected ERR_MAP_FULL");

    // 幂等
    ResolveOrCreateResult again;
    if (!PlacementStore::Instance().ReservePublicSlot(in51, &again) || !again.ok)
        return Fail("idempotent retry");
    if (again.placement.map_instance_id != out51.placement.map_instance_id)
        return Fail("idempotent different instance");
    if (!again.idempotent_hit)
        return Fail("idempotent_hit");

    // 释放补偿
    if (!PlacementStore::Instance().ReleaseByPlayer(2051))
        return Fail("release 51");
    if (PlacementStore::Instance().Occupancy(out51.placement.map_instance_id) != 0)
        return Fail("released occupancy not 0");

    // 100 并发，另一 template，容量 50
    const uint64_t tpl2 = 1002;
    constexpr int kN = 100;
    std::vector<uint64_t> got(kN, 0);
    std::atomic<int> ok{0};
    std::atomic<int> fail{0};
    std::vector<std::thread> th;
    th.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        th.emplace_back([&, i]() {
            ResolveOrCreateInput in;
            in.realm_id = realm;
            in.map_template_id = tpl2;
            in.player_id = 3000 + static_cast<uint64_t>(i);
            in.operation_id = "c-" + std::to_string(i);
            in.capacity = 50;
            ResolveOrCreateResult out;
            if (PlacementStore::Instance().ReservePublicSlot(in, &out) && out.ok) {
                got[static_cast<size_t>(i)] = out.placement.map_instance_id;
                ok.fetch_add(1);
            } else {
                fail.fetch_add(1);
            }
        });
    }
    for (auto &t : th)
        t.join();
    if (ok.load() != kN || fail.load() != 0)
        return Fail("concurrent reserve not all ok");
    std::unordered_map<uint64_t, int> counts;
    for (uint64_t id : got) {
        if (id == 0)
            return Fail("zero instance");
        counts[id] += 1;
    }
    for (const auto &kv : counts) {
        if (kv.second > 50)
            return Fail("instance over capacity");
        if (PlacementStore::Instance().Occupancy(kv.first) != static_cast<uint32_t>(kv.second))
            return Fail("occupancy mismatch");
    }
    if (counts.size() < 2)
        return Fail("expected at least 2 instances for 100/50");

    // 失败补偿：释放全部，容量归零
    for (int i = 0; i < kN; ++i)
        PlacementStore::Instance().ReleaseByPlayer(3000 + static_cast<uint64_t>(i));
    for (const auto &kv : counts) {
        if (PlacementStore::Instance().Occupancy(kv.first) != 0)
            return Fail("leak after release");
    }

    std::printf("OK map_occupancy_test instances=%zu\n", counts.size());
    return 0;
}
