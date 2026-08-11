#include "MapLeaseKeeper.h"

#include "Logging.h"
#include "MapInstanceRegistry.h"

#ifdef WEBSERVER_ENABLE_BRPC
#include "SessionRpcClient.h"
#endif
#ifdef WEBSERVER_ENABLE_REDIS
#include "PlacementStore.h"
#endif

MapLeaseKeeper &MapLeaseKeeper::Instance() {
    static MapLeaseKeeper g;
    return g;
}

void MapLeaseKeeper::SetLeaseSec(uint32_t sec) {
    if (sec > 0)
        lease_sec_ = sec;
}

void MapLeaseKeeper::Tick() {
    const size_t expired = MapInstanceRegistry::Instance().ReleaseExpired();
    if (expired > 0)
        LOG_WARN << "MapLeaseKeeper: released expired local maps n=" << expired;

    const auto owned = MapInstanceRegistry::Instance().ListOwned();
    if (owned.empty())
        return;

    const std::string owner = MapInstanceRegistry::Instance().local_instance_id();
#if defined(WEBSERVER_ENABLE_BRPC)
    if (SessionRpcClient::Instance().ready()) {
        for (const auto &p : owned) {
            // 空地图不续租，释放本地 Claim，避免 load 下心跳风暴拖垮 lease
            if (MapInstanceRegistry::Instance().PlayerCount(p.first) == 0) {
                MapInstanceRegistry::Instance().Release(p.first);
                continue;
            }
            int64_t until = 0;
            if (!SessionRpcClient::Instance().HeartbeatOwner(p.first, owner, p.second, lease_sec_,
                                                            &until)) {
                LOG_WARN << "MapLeaseKeeper: heartbeat failed map=" << p.first
                         << " epoch=" << p.second << " -> Release";
                MapInstanceRegistry::Instance().Release(p.first);
                continue;
            }
            if (until > 0)
                MapInstanceRegistry::Instance().SetLeaseUntil(p.first, until);
        }
        return;
    }
#endif
#if defined(WEBSERVER_ENABLE_REDIS)
    if (PlacementStore::Instance().Available()) {
        for (const auto &p : owned) {
            if (MapInstanceRegistry::Instance().PlayerCount(p.first) == 0) {
                MapInstanceRegistry::Instance().Release(p.first);
                continue;
            }
            int64_t until = 0;
            if (!PlacementStore::Instance().Heartbeat(p.first, owner, p.second, lease_sec_, &until)) {
                LOG_WARN << "MapLeaseKeeper: local Placement heartbeat failed map=" << p.first
                         << " -> Release";
                MapInstanceRegistry::Instance().Release(p.first);
                continue;
            }
            if (until > 0)
                MapInstanceRegistry::Instance().SetLeaseUntil(p.first, until);
        }
        return;
    }
#endif
    (void)owner;
}
