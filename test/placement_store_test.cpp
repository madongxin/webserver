/**
 * 阶段 2：权威 Placement（Redis Lua）
 */
#include "PlacementStore.h"
#include "RedisConfigPath.h"
#include "RedisPool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
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

bool InitRedisPoolFromCnf() {
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
    return RedisPool::Instance().Init(host, port, password, 4);
}

}  // namespace

int main() {
    if (!InitRedisPoolFromCnf()) {
        std::printf("FAIL placement_store_test (Redis unavailable)\n");
        return 1;
    }
    // 隔离前缀，避免与 E2E/正式拓扑共享 map:tpl 键导致并发唯一性假失败
    const std::string prefix =
        "gamemesh:test:placement_store:" + std::to_string(static_cast<unsigned long long>(::getpid())) +
        ":";
    if (!PlacementStore::Instance().InitFromSessionPrefix(prefix))
        return Fail("PlacementStore init");

    PlacementStore::Instance().SetLogicOwners({"gl-0", "gl-1"});

    // 并发创建同一 template → 单一权威实例
    constexpr int kN = 8;
    std::atomic<int> ok{0};
    std::vector<uint64_t> ids(static_cast<size_t>(kN), 0);
    std::vector<std::thread> th;
    const uint64_t tpl = 420000ULL + static_cast<uint64_t>(::getpid() % 100000);
    for (int i = 0; i < kN; ++i) {
        th.emplace_back([&, i]() {
            ResolveOrCreateInput in;
            in.realm_id = 1;
            in.map_template_id = tpl;
            ResolveOrCreateResult out;
            if (PlacementStore::Instance().ResolveOrCreate(in, &out) && out.ok) {
                ids[static_cast<size_t>(i)] = out.placement.map_instance_id;
                ok.fetch_add(1);
            }
        });
    }
    for (auto &t : th)
        t.join();
    if (ok.load() != kN)
        return Fail("concurrent resolve count");
    const uint64_t canon = ids[0];
    if (canon == 0)
        return Fail("empty id");
    for (int i = 1; i < kN; ++i) {
        if (ids[static_cast<size_t>(i)] != canon)
            return Fail("concurrent create not unique");
    }

    // force_new 可分配到不同 Logic
    ResolveOrCreateInput a;
    a.realm_id = 1;
    a.map_template_id = 100;
    a.force_new = true;
    a.preferred_owner = "gl-0";
    ResolveOrCreateResult ra;
    if (!PlacementStore::Instance().ResolveOrCreate(a, &ra) || !ra.ok)
        return Fail("force_new a");
    ResolveOrCreateInput b = a;
    b.preferred_owner = "gl-1";
    ResolveOrCreateResult rb;
    if (!PlacementStore::Instance().ResolveOrCreate(b, &rb) || !rb.ok)
        return Fail("force_new b");
    if (ra.placement.map_instance_id == rb.placement.map_instance_id)
        return Fail("force_new should differ");
    if (ra.placement.owner_logic_server_id != "gl-0" || rb.placement.owner_logic_server_id != "gl-1")
        return Fail("preferred owner");

    // 同一 Logic 多地图
    if (ra.placement.owner_logic_server_id == rb.placement.owner_logic_server_id) {
        // 若 RR 碰巧相同也允许，只要实例不同
    }

    // 活跃 lease 时 Migrate 应失败
    PlacementRecord blocked;
    std::string err;
    if (PlacementStore::Instance().Migrate(ra.placement.map_instance_id, "gl-1",
                                           ra.placement.owner_epoch, "mig-blocked", &blocked,
                                           &err))
        return Fail("migrate should fail while lease active");

    // Heartbeat 续租
    int64_t until = 0;
    if (!PlacementStore::Instance().Heartbeat(ra.placement.map_instance_id, "gl-0",
                                              ra.placement.owner_epoch, 30, &until) ||
        until <= 0)
        return Fail("heartbeat");

    // MarkRecovering 失效 lease 后可 Migrate
    PlacementRecord rec;
    if (!PlacementStore::Instance().MarkRecovering(ra.placement.map_instance_id, "kill", &rec))
        return Fail("recovering");
    if (rec.state != PlacementState::Recovering)
        return Fail("state recovering");
    if (PlacementStore::Instance().Heartbeat(ra.placement.map_instance_id, "gl-0",
                                             ra.placement.owner_epoch, 30, &until))
        return Fail("heartbeat must fail in RECOVERING");

    PlacementRecord mig;
    if (!PlacementStore::Instance().Migrate(ra.placement.map_instance_id, "gl-1",
                                            ra.placement.owner_epoch, "mig-1", &mig, &err))
        return Fail(("migrate after recovering: " + err).c_str());
    if (mig.owner_epoch <= ra.placement.owner_epoch)
        return Fail("epoch not bumped");
    PlacementRecord mig2;
    if (!PlacementStore::Instance().Migrate(ra.placement.map_instance_id, "gl-1", 0, "mig-1", &mig2,
                                            &err))
        return Fail("migrate idempotent");
    if (mig2.owner_epoch != mig.owner_epoch)
        return Fail("idempotent epoch changed");

    // rb：短 lease → 过期扫描 → Migrate
    int64_t short_until = 0;
    if (!PlacementStore::Instance().Heartbeat(rb.placement.map_instance_id, "gl-1",
                                              rb.placement.owner_epoch, 1, &short_until))
        return Fail("short heartbeat");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    PlacementRecord expired;
    if (!PlacementStore::Instance().ExpireLeaseToRecovering(rb.placement.map_instance_id, &expired,
                                                            &err))
        return Fail(("expire to recovering: " + err).c_str());
    if (expired.state != PlacementState::Recovering)
        return Fail("expire state");
    PlacementRecord mig_b;
    if (!PlacementStore::Instance().Migrate(rb.placement.map_instance_id, "gl-0", 0, "mig-b", &mig_b,
                                            &err))
        return Fail(("migrate after expire: " + err).c_str());

    // 模板复用 + lease 过期：ResolveOrCreate 必须 reclaim（刷新 lease/epoch），不得返回过期记录
    ResolveOrCreateInput expired_in;
    expired_in.realm_id = 1;
    expired_in.map_template_id = 880001ULL + static_cast<uint64_t>(::getpid() % 1000);
    expired_in.preferred_owner = "gl-0";
    ResolveOrCreateResult expired_a;
    if (!PlacementStore::Instance().ResolveOrCreate(expired_in, &expired_a) || !expired_a.ok)
        return Fail("expire-reclaim create");
    int64_t tiny = 0;
    if (!PlacementStore::Instance().Heartbeat(expired_a.placement.map_instance_id, "gl-0",
                                              expired_a.placement.owner_epoch, 1, &tiny))
        return Fail("expire-reclaim short hb");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    ResolveOrCreateResult expired_b;
    if (!PlacementStore::Instance().ResolveOrCreate(expired_in, &expired_b) || !expired_b.ok)
        return Fail("expire-reclaim resolve");
    if (expired_b.placement.map_instance_id != expired_a.placement.map_instance_id)
        return Fail("expire-reclaim id changed");
    // READY+过期且 Owner 仍健康：软续租，epoch 保持不变
    if (expired_b.placement.owner_epoch != expired_a.placement.owner_epoch)
        return Fail("expire-reclaim soft renew should keep epoch");
    if (expired_b.placement.owner_logic_server_id != expired_a.placement.owner_logic_server_id)
        return Fail("expire-reclaim owner changed");
    const int64_t now =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (expired_b.placement.lease_until <= now)
        return Fail("expire-reclaim lease still stale");
    if (expired_b.placement.state != PlacementState::Ready)
        return Fail("expire-reclaim not READY");

    // READY+过期但 Owner 已不在健康列表：硬 reclaim（升 epoch，换新 Owner）
    {
        ResolveOrCreateInput dead_in;
        dead_in.realm_id = 1;
        dead_in.map_template_id = expired_in.map_template_id + 7;
        dead_in.preferred_owner = "gl-0";
        ResolveOrCreateResult dead_a;
        if (!PlacementStore::Instance().ResolveOrCreate(dead_in, &dead_a) || !dead_a.ok)
            return Fail("dead-owner create");
        int64_t tiny2 = 0;
        if (!PlacementStore::Instance().Heartbeat(dead_a.placement.map_instance_id, "gl-0",
                                                  dead_a.placement.owner_epoch, 1, &tiny2))
            return Fail("dead-owner short hb");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        PlacementStore::Instance().SetLogicOwners({"gl-1"});  // gl-0 视为死亡
        ResolveOrCreateResult dead_b;
        if (!PlacementStore::Instance().ResolveOrCreate(dead_in, &dead_b) || !dead_b.ok)
            return Fail("dead-owner resolve");
        if (dead_b.placement.map_instance_id != dead_a.placement.map_instance_id)
            return Fail("dead-owner id changed");
        if (dead_b.placement.owner_epoch <= dead_a.placement.owner_epoch)
            return Fail("dead-owner epoch not bumped");
        if (dead_b.placement.owner_logic_server_id != "gl-1")
            return Fail("dead-owner not reclaimed to healthy");
        if (dead_b.placement.route_version <= dead_a.placement.route_version)
            return Fail("dead-owner route_version not bumped");
        PlacementStore::Instance().SetLogicOwners({"gl-0", "gl-1"});
    }

    // 健康 Owner 清空：新建 Placement fail-closed
    {
        PlacementStore::Instance().SetLogicOwners({});
        ResolveOrCreateInput no_in;
        no_in.realm_id = 1;
        no_in.map_template_id = expired_in.map_template_id + 99;
        ResolveOrCreateResult no_out;
        if (PlacementStore::Instance().ResolveOrCreate(no_in, &no_out))
            return Fail("empty-owners resolve should fail");
        if (no_out.error_code != "NO_HEALTHY_GAMELOGIC")
            return Fail("empty-owners error_code");
        if (!PlacementStore::Instance().PickHealthyOwner("").empty())
            return Fail("empty-owners PickHealthyOwner");
        PlacementStore::Instance().SetLogicOwners({"gl-0", "gl-1"});
    }

    // 孤儿 tpl 索引（inst 已删）：Resolve 必须重建可用 Placement，不能返回幽灵 id
    {
        ResolveOrCreateInput orphan_in;
        orphan_in.realm_id = 1;
        orphan_in.map_template_id = expired_in.map_template_id + 123;
        ResolveOrCreateResult orphan_a;
        if (!PlacementStore::Instance().ResolveOrCreate(orphan_in, &orphan_a) || !orphan_a.ok)
            return Fail("orphan-setup create");
        const uint64_t ghost = orphan_a.placement.map_instance_id;
        // 模拟 inst TTL 过期但 tpl 索引残留
        {
            auto lease = RedisPool::Instance().Acquire();
            if (!lease)
                return Fail("orphan-setup redis");
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%smap:inst:%llu", prefix.c_str(),
                          static_cast<unsigned long long>(ghost));
            lease->Del(buf);
        }
        ResolveOrCreateResult orphan_b;
        if (!PlacementStore::Instance().ResolveOrCreate(orphan_in, &orphan_b) || !orphan_b.ok)
            return Fail("orphan-resolve");
        PlacementRecord got;
        if (!PlacementStore::Instance().Get(orphan_b.placement.map_instance_id, &got))
            return Fail("orphan-resolve ghost id not readable");
        if (got.state != PlacementState::Ready)
            return Fail("orphan-resolve not READY");
    }

    // RECOVERING：硬 reclaim 升 epoch
    ResolveOrCreateInput hard_in = expired_in;
    hard_in.map_template_id = expired_in.map_template_id + 1;
    ResolveOrCreateResult hard_a;
    if (!PlacementStore::Instance().ResolveOrCreate(hard_in, &hard_a) || !hard_a.ok)
        return Fail("hard-reclaim create");
    PlacementRecord hard_rec;
    if (!PlacementStore::Instance().MarkRecovering(hard_a.placement.map_instance_id, "t", &hard_rec))
        return Fail("hard-reclaim mark");
    ResolveOrCreateResult hard_b;
    if (!PlacementStore::Instance().ResolveOrCreate(hard_in, &hard_b) || !hard_b.ok)
        return Fail("hard-reclaim resolve");
    if (hard_b.placement.owner_epoch <= hard_a.placement.owner_epoch)
        return Fail("hard-reclaim epoch not bumped");
    if (hard_b.placement.state != PlacementState::Ready)
        return Fail("hard-reclaim not READY");

    std::printf("OK placement_store_test concurrent/unique/lease/migrate/recover\n");
    std::printf("PASS placement_store_test\n");
    return 0;
}
