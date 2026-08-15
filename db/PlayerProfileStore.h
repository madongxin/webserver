#pragma once

/**
 * @file PlayerProfileStore.h
 * @brief 玩家资料表 player_profile（仅 GameDB/World 写；GameLogic 经 RPC 读）
 */

#include <cstdint>
#include <string>

class Connection;

struct PlayerProfileRow {
    uint64_t player_id = 0;
    std::string player_name;
    int32_t hp = 100;
    int32_t max_hp = 100;
    int32_t mp = 100;
    int32_t max_mp = 100;
    int32_t attack = 10;
    int32_t spell_power = 10;
    int32_t defense = 5;
    int32_t magic_resistance = 5;
    float crit_chance = 0.05f;
    float crit_damage = 1.5f;
    float move_speed = 10.0f;
    float attack_speed = 1.0f;
    uint64_t stats_version = 1;
    bool exists = false;
};

class PlayerProfileStore {
public:
    static PlayerProfileStore &Instance();

    bool EnsureTable();
    bool Available() const { return available_; }

    /** 注册事务内插入默认档；已存在则忽略（幂等）。 */
    bool InsertDefaultOnConnection(Connection *conn, uint64_t player_id,
                                   const std::string &player_name, std::string *err);

    /** 老账号缺档：独立连接 INSERT IGNORE 默认档。 */
    bool EnsureDefault(uint64_t player_id, const std::string &player_name, std::string *err);

    bool Load(uint64_t player_id, PlayerProfileRow *out, std::string *err);

    /**
     * 版本保护保存。expected_stats_version==0 不校验。
     * 成功后 *out_version 为新版本。
     */
    bool Save(const PlayerProfileRow &row, uint64_t expected_stats_version, uint64_t *out_version,
              std::string *err, std::string *err_code);

    static bool Validate(const PlayerProfileRow &row, std::string *err_code, std::string *err);
    static void FillDefaults(uint64_t player_id, const std::string &player_name,
                             PlayerProfileRow *out);

private:
    PlayerProfileStore() = default;
    bool available_ = false;
    bool table_ready_ = false;
};
