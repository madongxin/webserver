#pragma once

/**
 * @file PlayerAccountStore.h
 * @brief 玩家账号注册表 player_account（仅 GameDB 进程应写入；Auth 经 GameDB RPC）
 */

#include <cstdint>
#include <string>

struct AccountAuthRow {
    uint64_t player_id = 0;
    uint64_t account_id = 0;
    bool exists = false;
    bool banned = false;
    std::string password_hash;
    std::string password_salt;
    int password_iters = 0;
    bool has_password = false;
};

class PlayerAccountStore {
public:
    static PlayerAccountStore &Instance();

    bool EnsureTable();
    bool Available() const { return available_; }

    /** 兼容旧无密码注册 */
    bool Register(const std::string &device_id, const std::string &display_name, uint64_t *player_id,
                  std::string *err);

    bool RegisterWithPassword(const std::string &device_id, const std::string &display_name,
                              const std::string &password_hash, const std::string &password_salt,
                              int password_iters, uint64_t *player_id, std::string *err);

    bool Exists(uint64_t player_id);
    bool LoadAuth(uint64_t player_id, AccountAuthRow *out);

private:
    PlayerAccountStore() = default;
    bool available_ = false;
    bool table_ready_ = false;
};
