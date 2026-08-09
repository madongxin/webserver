#include "PlacementRecoveryScheduler.h"

#include "Logging.h"
#include "PlacementStore.h"

#include <string>
#include <vector>

PlacementRecoveryScheduler &PlacementRecoveryScheduler::Instance() {
    static PlacementRecoveryScheduler g;
    return g;
}

void PlacementRecoveryScheduler::SetScanCount(size_t n) {
    if (n > 0)
        scan_count_ = n;
}

void PlacementRecoveryScheduler::Tick() {
    if (!PlacementStore::Instance().Available())
        return;

    std::vector<uint64_t> expired;
    std::vector<uint64_t> recovering;
    if (!PlacementStore::Instance().ScanRecoveryCandidates(&cursor_, scan_count_, &expired,
                                                           &recovering)) {
        cursor_ = "0";
        return;
    }

    for (uint64_t mid : expired) {
        PlacementRecord before;
        PlacementStore::Instance().Get(mid, &before);
        PlacementRecord after;
        std::string err;
        if (!PlacementStore::Instance().ExpireLeaseToRecovering(mid, &after, &err)) {
            ++recover_fail_;
            LOG_WARN << "PlacementRecovery: expire failed map=" << mid << " err=" << err;
            continue;
        }
        PlacementStore::Instance().AppendAudit(mid, "EXPIRE_RECOVERING", before.owner_logic_server_id,
                                               after.owner_logic_server_id, before.owner_epoch,
                                               after.owner_epoch, "lease_expired");
        recovering.push_back(mid);
    }

    for (uint64_t mid : recovering) {
        PlacementRecord cur;
        if (!PlacementStore::Instance().Get(mid, &cur))
            continue;
        if (cur.state != PlacementState::Recovering)
            continue;
        const std::string new_owner =
            PlacementStore::Instance().PickHealthyOwner(cur.owner_logic_server_id);
        if (new_owner.empty() || new_owner == cur.owner_logic_server_id) {
            ++recover_fail_;
            LOG_WARN << "PlacementRecovery: no alternate owner map=" << mid
                     << " stuck_owner=" << cur.owner_logic_server_id;
            continue;
        }
        PlacementRecord out;
        std::string err;
        const std::string idem = "auto-recover:" + std::to_string(mid) + ":" +
                                 std::to_string(cur.owner_epoch) + ":" + new_owner;
        if (!PlacementStore::Instance().Migrate(mid, new_owner, cur.owner_epoch, idem, &out, &err)) {
            ++recover_fail_;
            LOG_WARN << "PlacementRecovery: migrate failed map=" << mid << " -> " << new_owner
                     << " err=" << err;
            PlacementStore::Instance().AppendAudit(mid, "MIGRATE_FAIL", cur.owner_logic_server_id,
                                                   new_owner, cur.owner_epoch, cur.owner_epoch, err);
            continue;
        }
        ++recover_ok_;
        PlacementStore::Instance().AppendAudit(mid, "MIGRATE_OK", cur.owner_logic_server_id,
                                               out.owner_logic_server_id, cur.owner_epoch,
                                               out.owner_epoch, "auto_scheduler");
        LOG_WARN << "PlacementRecovery: map=" << mid << " " << cur.owner_logic_server_id << "@"
                 << cur.owner_epoch << " -> " << out.owner_logic_server_id << "@" << out.owner_epoch
                 << " (客户端需重新进图/安全点，非无损实时恢复)";
    }
}
