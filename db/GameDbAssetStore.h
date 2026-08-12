#pragma once

#include "Connection.h"

#include <cstdint>
#include <map>
#include <string>

/** GameDB 权威资产：版本 CAS + 幂等键（MySQL） */
class GameDbAssetStore {
public:
    static GameDbAssetStore &Instance();

    bool EnsureTables();
    bool Available() const { return available_; }

    bool LoadMeta(uint64_t player_id, uint64_t *version, bool *exists);
    bool LoadInventory(uint64_t player_id, std::map<uint32_t, uint32_t> *bag, uint64_t *version);

    /** 幂等键：1..128，禁止控制字符；超长/非法返回 false */
    static bool ValidateIdempotencyKey(const std::string &key, std::string *err);

    struct MutationResult {
        bool ok = false;
        bool idempotent_hit = false;
        std::string error_code;
        std::string message;
        uint64_t asset_version = 0;
        uint32_t remain_count = 0;
    };
    bool ApplyMutation(uint64_t player_id, const std::string &idempotency_key,
                       uint64_t expected_version, const std::string &mutation_type, uint32_t item_id,
                       uint32_t count, MutationResult *out);

    /**
     * 在已有事务连接上 GRANT 多个道具并 bump asset_version（不 Begin/Commit）。
     * 用于邮件领取与正式背包同一 MySQL 事务提交。
     * @param expected_version 0 表示不校验；否则必须匹配当前 meta
     * @param soft_cap 单道具数量软上限；<=0 表示不检查
     */
    bool GrantItemsOnConnection(Connection *conn, uint64_t player_id, uint64_t expected_version,
                                const std::map<uint32_t, uint32_t> &grants, int64_t soft_cap,
                                uint64_t *new_version, std::string *err_code, std::string *err_msg);

    struct SnapshotResult {
        bool ok = false;
        bool idempotent_hit = false;
        std::string error_code;
        std::string message;
        uint64_t asset_version = 0;
    };
    bool SaveSnapshot(uint64_t player_id, uint64_t expected_version,
                      const std::map<uint32_t, uint32_t> &bag, const std::string &idempotency_key,
                      SnapshotResult *out);

    /** status: NOT_FOUND | IN_PROGRESS | SUCCEEDED | FAILED */
    struct OperationQuery {
        bool found = false;
        bool completed_ok = false;
        std::string status;
        std::string error_code;
        std::string message;
        uint64_t asset_version = 0;
        uint32_t remain_count = 0;
        std::string request_hash;
        std::string operation_type;
    };
    bool QueryOperationResult(uint64_t player_id, const std::string &idempotency_key,
                              const std::string &operation_type, OperationQuery *out);

private:
    GameDbAssetStore() = default;
    bool available_ = false;
    bool tables_ready_ = false;
};
