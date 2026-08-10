#pragma once

#include <cstddef>
#include <string>

/**
 * Session 进程后台：扫描过期 Placement lease → RECOVERING → Migrate 到健康 Owner。
 * 多 Session 实例时通过 Redis leader lease（CAS）互斥，避免双接管。
 * GameLogic 侧 MapLeaseKeeper 负责续租；本调度器负责失联接管。
 * 恢复策略：无实时地图快照时重建权威 Placement，客户端需重新进图（非无损）。
 */
class PlacementRecoveryScheduler {
public:
    static PlacementRecoveryScheduler &Instance();

    void SetScanCount(size_t n);
    /** 本 Session 实例 id，用于 leader lease；空则跳过接管（fail-closed 多实例） */
    void SetInstanceId(const std::string &id);
    void SetKeyPrefix(const std::string &prefix);
    void SetLeaderLeaseSec(int sec);

    /** EventLoop 周期调用 */
    void Tick();

    uint64_t recover_ok() const { return recover_ok_; }
    uint64_t recover_fail() const { return recover_fail_; }
    uint64_t leader_skip() const { return leader_skip_; }
    bool is_leader() const { return is_leader_; }

private:
    PlacementRecoveryScheduler() = default;
    bool TryAcquireOrRenewLeader();

    size_t scan_count_ = 32;
    std::string cursor_ = "0";
    std::string instance_id_;
    std::string key_prefix_ = "gamemesh:dev:";
    int leader_lease_sec_ = 15;
    bool is_leader_ = false;
    uint64_t recover_ok_ = 0;
    uint64_t recover_fail_ = 0;
    uint64_t leader_skip_ = 0;
};
