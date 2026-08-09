#pragma once

#include <cstddef>
#include <string>

/**
 * Session 进程后台：扫描过期 Placement lease → RECOVERING → Migrate 到健康 Owner。
 * GameLogic 侧 MapLeaseKeeper 负责续租；本调度器负责失联接管。
 */
class PlacementRecoveryScheduler {
public:
    static PlacementRecoveryScheduler &Instance();

    void SetScanCount(size_t n);
    /** EventLoop 周期调用 */
    void Tick();

    uint64_t recover_ok() const { return recover_ok_; }
    uint64_t recover_fail() const { return recover_fail_; }

private:
    PlacementRecoveryScheduler() = default;
    size_t scan_count_ = 32;
    std::string cursor_ = "0";
    uint64_t recover_ok_ = 0;
    uint64_t recover_fail_ = 0;
};
