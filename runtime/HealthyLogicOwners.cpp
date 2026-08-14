#include "HealthyLogicOwners.h"

#include "HealthyLogicSnapshot.h"
#include "IServiceRegistry.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PlacementStore.h"
#include "RedisServiceRegistry.h"
#include "SessionStore.h"

#include <string>
#include <vector>

namespace {

void ApplyIds(const std::vector<std::string> &ids) {
    SessionStore::Instance().SetLogicInstanceIds(ids, false);
    PlacementStore::Instance().SetLogicOwners(ids, false);
}

}  // namespace

HealthyLogicRefreshResult RefreshHealthyLogicOwners(bool update_static_addrs) {
    HealthyLogicRefreshResult r;
    if (!RedisServiceRegistry::Get().ready()) {
        r.status = HealthyLogicRefreshStatus::kNotReady;
        OpsMetrics::Instance().IncLogicDiscoverNotReady();
        return r;
    }
    std::vector<IServiceRegistry::ServiceInstance> insts;
    if (!RedisServiceRegistry::Get().Discover("gamelogic", &insts)) {
        r.status = HealthyLogicRefreshStatus::kDiscoverFailed;
        OpsMetrics::Instance().IncLogicDiscoverFail();
        LOG_ERROR << "HealthyLogicOwners: Discover failed (keep snapshot)";
        return r;
    }

    std::vector<std::string> ids;
    std::vector<std::string> static_ids;
    std::vector<std::string> static_addrs;
    ids.reserve(insts.size());
    static_ids.reserve(insts.size());
    static_addrs.reserve(insts.size());
    for (const auto &i : insts) {
        if (i.instance_id.empty())
            continue;
        ids.push_back(i.instance_id);
        if (!i.address.empty()) {
            static_ids.push_back(i.instance_id);
            static_addrs.push_back(i.address);
        }
    }
    r.status = HealthyLogicRefreshStatus::kApplied;
    r.instance_count = ids.size();

    auto snap = std::make_shared<HealthyLogicSnapshot>();
    snap->source = HealthyLogicSnapshot::Source::kRegistry;
    auto cur = HealthyLogicSnapshotStore::Instance().Current();
    const bool seen_active = cur && cur->state == HealthyLogicSnapshot::State::kActive;

    if (ids.empty()) {
        const bool have_bootstrap =
            cur && !cur->instance_ids.empty() && cur->state != HealthyLogicSnapshot::State::kActive &&
            cur->state != HealthyLogicSnapshot::State::kEmpty;
        if (!seen_active && have_bootstrap) {
            OpsMetrics::Instance().IncLogicDiscoverNotReady();
            r.status = HealthyLogicRefreshStatus::kNotReady;
            LOG_WARN << "HealthyLogicOwners: Discover empty before first healthy set; "
                        "keep bootstrap snapshot";
            return r;
        }
        snap->state = HealthyLogicSnapshot::State::kEmpty;
        snap->instance_ids.clear();
        HealthyLogicSnapshotStore::Instance().Publish(snap);
        ApplyIds({});
        OpsMetrics::Instance().IncLogicDiscoverEmpty();
        LOG_WARN << "HealthyLogicOwners: Discover empty -> fail-closed "
                    "(NO_HEALTHY_GAMELOGIC)";
        return r;
    }
    snap->state = HealthyLogicSnapshot::State::kActive;
    snap->instance_ids = ids;
    if (!HealthyLogicSnapshotStore::Instance().Publish(snap)) {
        LOG_WARN << "HealthyLogicOwners: stale version ignored";
        return r;
    }
    ApplyIds(ids);
    OpsMetrics::Instance().IncLogicDiscoverOk();
    LOG_INFO << "HealthyLogicOwners: replaced count=" << ids.size()
             << " version=" << HealthyLogicSnapshotStore::Instance().version();
    if (update_static_addrs)
        StaticServiceRegistry::Get().SetStaticAddrs("gamelogic", static_addrs, static_ids);
    return r;
}
