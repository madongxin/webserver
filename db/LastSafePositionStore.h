#pragma once

/**
 * @file LastSafePositionStore.h
 * @brief 最后安全位置表（仅 GameDB 写 MySQL；GameLogic 经 brpc）
 */

#include <cstdint>
#include <string>

class Connection;

struct LastSafePositionRow {
    uint64_t player_id = 0;
    uint32_t realm_id = 1;
    uint64_t map_template_id = 0;
    float x = 0;
    float y = 0;
    float z = 0;
    float yaw = 0;
    uint64_t position_version = 0;
    bool exists = false;
};

class LastSafePositionStore {
public:
    static LastSafePositionStore &Instance();

    bool EnsureTable();
    bool Available() const { return available_; }

    bool Load(uint64_t player_id, LastSafePositionRow *out, std::string *err);

    /**
     * 版本保护保存。expected_version==0 不校验。
     * 非法坐标（非有限）拒绝且不落库。
     */
    bool Save(const LastSafePositionRow &row, uint64_t expected_version, uint64_t *out_version,
              bool *skipped, std::string *err, std::string *err_code);

    static bool ValidateFinite(const LastSafePositionRow &row, std::string *err_code,
                               std::string *err);

private:
    LastSafePositionStore() = default;
    bool available_ = false;
    bool table_ready_ = false;
};
