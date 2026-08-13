#include "HealthyLogicOwners.h"

#include "IServiceRegistry.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "PlacementStore.h"
#include "RedisServiceRegistry.h"
#include "SessionStore.h"

#include <string>
#include <vector>

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
    SessionStore::Instance().SetLogicInstanceIds(ids);
    PlacementStore::Instance().SetLogicOwners(ids);
    if (ids.empty()) {
        OpsMetrics::Instance().IncLogicDiscoverEmpty();
        LOG_WARN << "HealthyLogicOwners: Discover empty -> fail-closed "
                    "(NO_HEALTHY_GAMELOGIC)";
    } else {
        OpsMetrics::Instance().IncLogicDiscoverOk();
        LOG_INFO << "HealthyLogicOwners: replaced count=" << ids.size();
        if (update_static_addrs)
            StaticServiceRegistry::Get().SetStaticAddrs("gamelogic", static_addrs, static_ids);
    }
    return r;
}
