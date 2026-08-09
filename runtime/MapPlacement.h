#pragma once

/**
 * @file MapPlacement.h
 * @brief 地图 Placement 表（阶段 5 骨架）：map_instance → owner + epoch + route_version
 *
 * 进程内权威（Gateway / role=all）；多 Gateway 共享需后续 Redis。无 AOI/Tick。
 */

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct MapPlacementRecord {
    uint32_t realm_id = 0;
    uint64_t map_template_id = 0;
    uint64_t map_instance_id = 0;
    std::string owner_gamelogic_id;
    uint64_t owner_epoch = 0;
    uint64_t route_version = 0;
    int64_t lease_until_unix = 0;
    bool frozen = false;
};

class MapPlacement {
public:
    static MapPlacement &Instance();

    /** 可放置的同构 Logic 实例 ID 列表（与 gateway logic_addrs 对齐） */
    void ConfigureOwners(std::vector<std::string> owner_ids);

    const std::vector<std::string> &owners() const { return owners_; }

    /**
     * map_instance_id==0 时新建并 RR 选 owner；否则查表。
     * @return false 查无或 owners 为空
     */
    bool ResolveOrAllocate(uint32_t realm_id, uint64_t map_template_id, uint64_t map_instance_id,
                           MapPlacementRecord *out);

    bool Get(uint64_t map_instance_id, MapPlacementRecord *out) const;

    /**
     * 骨架迁移：换 owner、epoch+1、route_version+1；旧 epoch 写应被 Logic 拒绝。
     */
    bool Migrate(uint64_t map_instance_id, const std::string &new_owner,
                 MapPlacementRecord *out);

    /** 用权威 Placement 刷新本地缓存（非事实源） */
    void UpsertCache(const MapPlacementRecord &rec);

    void ClearForTest();

private:
    MapPlacement() = default;

    mutable std::mutex mu_;
    std::vector<std::string> owners_;
    size_t rr_ = 0;
    uint64_t next_instance_id_ = 1;
    std::unordered_map<uint64_t, MapPlacementRecord> table_;
};
