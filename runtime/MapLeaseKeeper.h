#pragma once

#include <cstdint>

/**
 * GameLogic 侧 Owner lease 续租：对本地 Claim 的地图调用 Session HeartbeatOwner。
 * 续租失败或本地 lease 过期 → Release（fail-closed）。
 */
class MapLeaseKeeper {
public:
    static MapLeaseKeeper &Instance();

    void SetLeaseSec(uint32_t sec);
    uint32_t lease_sec() const { return lease_sec_; }

    /** 由 EventLoop 周期调用 */
    void Tick();

private:
    MapLeaseKeeper() = default;
    uint32_t lease_sec_ = 30;
};
