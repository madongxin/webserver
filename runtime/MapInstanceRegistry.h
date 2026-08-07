#pragma once

/**
 * @file MapInstanceRegistry.h
 * @brief GameLogic 本地地图实例注册：Claim + epoch 写栅栏（无 AOI/Tick）
 */

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct LocalMapInstance {
    uint64_t map_template_id = 0;
    uint64_t map_instance_id = 0;
    uint64_t owner_epoch = 0;
    std::unordered_set<uint64_t> players;
};

class MapInstanceRegistry {
public:
    static MapInstanceRegistry &Instance();

    void SetLocalInstanceId(std::string id);
    const std::string &local_instance_id() const { return local_id_; }

    /** Claim / 升 epoch；同 epoch 幂等成功；更低 epoch 拒绝 */
    bool Claim(uint64_t map_instance_id, uint64_t map_template_id, uint64_t owner_epoch);

    /** 请求 epoch 必须等于本地 Claim 的 epoch */
    bool AcceptWrite(uint64_t map_instance_id, uint64_t owner_epoch) const;

    bool Has(uint64_t map_instance_id) const;
    uint64_t Epoch(uint64_t map_instance_id) const;

    bool AddPlayer(uint64_t map_instance_id, uint64_t player_id);
    bool RemovePlayer(uint64_t map_instance_id, uint64_t player_id);
    bool PlayerOnMap(uint64_t map_instance_id, uint64_t player_id) const;
    uint32_t PlayerCount(uint64_t map_instance_id) const;

    /** 放弃本地权威（迁移后旧 Owner 调用） */
    void Release(uint64_t map_instance_id);

    void ClearForTest();

private:
    MapInstanceRegistry() = default;

    mutable std::mutex mu_;
    std::string local_id_ = "gl-local";
    std::unordered_map<uint64_t, LocalMapInstance> maps_;
};
