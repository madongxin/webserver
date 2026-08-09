#pragma once

/**
 * @file MapInstanceRegistry.h
 * @brief GameLogic 本地地图实例：Claim + epoch + owner lease 写栅栏
 */

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct LocalMapInstance {
    uint64_t map_template_id = 0;
    uint64_t map_instance_id = 0;
    uint64_t owner_epoch = 0;
    /** Unix 秒；0 = 未启用本地 lease（仅测试/无 Placement） */
    int64_t lease_until_unix = 0;
    std::unordered_set<uint64_t> players;
};

enum class MapWriteFence {
    Ok = 0,
    NotClaimed,
    StaleEpoch,
    LeaseExpired,
};

class MapInstanceRegistry {
public:
    static MapInstanceRegistry &Instance();

    void SetLocalInstanceId(std::string id);
    const std::string &local_instance_id() const { return local_id_; }

    /** Claim / 升 epoch；同 epoch 幂等；更低 epoch 拒绝。lease_until_unix=0 表示不启用本地 lease。 */
    bool Claim(uint64_t map_instance_id, uint64_t map_template_id, uint64_t owner_epoch,
               int64_t lease_until_unix = 0);

    void SetLeaseUntil(uint64_t map_instance_id, int64_t lease_until_unix);
    int64_t LeaseUntil(uint64_t map_instance_id) const;

    MapWriteFence CheckWrite(uint64_t map_instance_id, uint64_t owner_epoch) const;
    /** 请求 epoch 必须等于本地 Claim，且本地 lease 未过期（lease=0 跳过） */
    bool AcceptWrite(uint64_t map_instance_id, uint64_t owner_epoch) const;

    bool Has(uint64_t map_instance_id) const;
    uint64_t Epoch(uint64_t map_instance_id) const;

    bool AddPlayer(uint64_t map_instance_id, uint64_t player_id);
    bool RemovePlayer(uint64_t map_instance_id, uint64_t player_id);
    bool PlayerOnMap(uint64_t map_instance_id, uint64_t player_id) const;
    uint32_t PlayerCount(uint64_t map_instance_id) const;

    /** 放弃本地权威（迁移后旧 Owner / lease 失效） */
    void Release(uint64_t map_instance_id);

    /** 释放所有本地 lease 已过期的地图；返回释放数量 */
    size_t ReleaseExpired();

    /** (map_instance_id, owner_epoch) 列表，供续租 */
    std::vector<std::pair<uint64_t, uint64_t>> ListOwned() const;

    void ClearForTest();

private:
    MapInstanceRegistry() = default;
    static int64_t NowUnixSec();

    mutable std::mutex mu_;
    std::string local_id_ = "gl-local";
    std::unordered_map<uint64_t, LocalMapInstance> maps_;
};
