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

    /**
     * 带幂等键注册：相同 idempotency_key 重试返回首次 player_id，不重复建号。
     * idempotency_key 空则退化为非幂等 RegisterWithPassword。
     */
    bool RegisterWithPasswordIdempotent(const std::string &device_id, const std::string &display_name,
                                        const std::string &password_hash,
                                        const std::string &password_salt, int password_iters,
                                        const std::string &idempotency_key, uint64_t *player_id,
                                        std::string *err, bool *replayed = nullptr);

    bool Exists(uint64_t player_id);
    bool LoadAuth(uint64_t player_id, AccountAuthRow *out);
    bool FindByIdempotencyKey(const std::string &idempotency_key, uint64_t *player_id);

private:
    PlayerAccountStore() = default;
    bool available_ = false;
    bool table_ready_ = false;
};
