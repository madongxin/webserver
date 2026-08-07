#pragma once

/**
 * @file PlayerAccountStore.h
 * @brief 玩家账号注册表 player_account（分配 player_id）
 */

#include <cstdint>
#include <string>

class PlayerAccountStore {
public:
    static PlayerAccountStore &Instance();

    bool EnsureTable();
    bool Available() const { return available_; }

    /** 注册新账号，成功写出 player_id */
    bool Register(const std::string &device_id, const std::string &display_name, uint64_t *player_id,
                  std::string *err);
    bool Exists(uint64_t player_id);

private:
    PlayerAccountStore() = default;
    bool available_ = false;
    bool table_ready_ = false;
};
