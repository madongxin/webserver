#include "PlacementRecoveryScheduler.h"

#include "Logging.h"
#include "OpsMetrics.h"
#include "PlacementStore.h"
#include "RedisPool.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

// KEYS[1]=lease_key ARGV[1]=instance_id ARGV[2]=ttl_sec
// 持有者续租，或 NX 抢占；返回 1=leader 0=follower
constexpr const char *kLeaderScript = R"LUA(
local cur = redis.call('GET', KEYS[1])
if cur == ARGV[1] then
  redis.call('EXPIRE', KEYS[1], tonumber(ARGV[2]))
  return 1
end
if cur == false then
  local ok = redis.call('SET', KEYS[1], ARGV[1], 'EX', tonumber(ARGV[2]), 'NX')
  if ok then return 1 end
  return 0
end
return 0
)LUA";

}  // namespace

PlacementRecoveryScheduler &PlacementRecoveryScheduler::Instance() {
    static PlacementRecoveryScheduler g;
    return g;
}

void PlacementRecoveryScheduler::SetScanCount(size_t n) {
    if (n > 0)
        scan_count_ = n;
}

void PlacementRecoveryScheduler::SetInstanceId(const std::string &id) {
    instance_id_ = id;
}

void PlacementRecoveryScheduler::SetKeyPrefix(const std::string &prefix) {
    if (!prefix.empty())
        key_prefix_ = prefix;
    cursor_ = "0";
}

void PlacementRecoveryScheduler::SetLeaderLeaseSec(int sec) {
    if (sec > 0)
        leader_lease_sec_ = sec;
}

bool PlacementRecoveryScheduler::TryAcquireOrRenewLeader() {
    if (instance_id_.empty()) {
        if (const char *env = std::getenv("GAMEMESH_INSTANCE_ID"))
            instance_id_ = env;
    }
    if (instance_id_.empty()) {
        // 单测/单实例兼容：无 id 时允许跑（视为单领导者）
        is_leader_ = true;
        return true;
    }
    if (!RedisPool::Instance().ready()) {
        is_leader_ = false;
        return false;
    }
    auto lease = RedisPool::Instance().Acquire();
    if (!lease) {
        is_leader_ = false;
        return false;
    }
    const std::string key = key_prefix_ + "placement:recovery:leader";
    std::vector<std::string> out;
    if (!lease->Eval(kLeaderScript, {key}, {instance_id_, std::to_string(leader_lease_sec_)},
                     &out)) {
        is_leader_ = false;
        return false;
    }
    is_leader_ = !out.empty() && (out[0] == "1" || out[0] == "1.0");
    return is_leader_;
}

void PlacementRecoveryScheduler::Tick() {
    if (!PlacementStore::Instance().Available())
        return;

    if (!TryAcquireOrRenewLeader()) {
        ++leader_skip_;
        return;
    }

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
            OpsMetrics::Instance().IncPlacementRecoverFail();
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
            OpsMetrics::Instance().IncPlacementRecoverFail();
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
            OpsMetrics::Instance().IncPlacementRecoverFail();
            LOG_WARN << "PlacementRecovery: migrate failed map=" << mid << " -> " << new_owner
                     << " err=" << err;
            PlacementStore::Instance().AppendAudit(mid, "MIGRATE_FAIL", cur.owner_logic_server_id,
                                                   new_owner, cur.owner_epoch, cur.owner_epoch, err);
            continue;
        }
        ++recover_ok_;
        OpsMetrics::Instance().IncPlacementRecoverOk();
        PlacementStore::Instance().AppendAudit(mid, "MIGRATE_OK", cur.owner_logic_server_id,
                                               out.owner_logic_server_id, cur.owner_epoch,
                                               out.owner_epoch, "auto_scheduler");
        LOG_WARN << "PlacementRecovery: map=" << mid << " " << cur.owner_logic_server_id << "@"
                 << cur.owner_epoch << " -> " << out.owner_logic_server_id << "@" << out.owner_epoch
                 << " (客户端需重新进图/安全点，非无损实时恢复)";
    }
}
