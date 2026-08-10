#pragma once

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

    struct OperationQuery {
        bool found = false;
        bool completed_ok = false;
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
