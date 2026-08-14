#pragma once

#include "IGameDbRepository.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace brpc {
class Channel;
}

struct GameDbChannelSnapshot {
    std::vector<std::shared_ptr<brpc::Channel>> channels;
    std::vector<std::string> addrs;
    uint64_t version = 0;
};

/** World/Logic 侧：经 brpc 调用独立 GameDB；支持多地址 + ApplySnapshot */
class BrpcGameDbRepository : public IGameDbRepository {
public:
    static BrpcGameDbRepository &Instance();

    bool Init(const std::string &addr, int timeout_ms = 3000);
    bool Init(const std::vector<std::string> &addrs, int timeout_ms = 3000);
    /** 热更新 Channel 快照；空列表保留现有（不 fail-open 清空） */
    bool ApplySnapshot(const std::vector<std::string> &addrs);
    void Start(int worker_count = 0) override;
    void Stop() override;
    bool started() const override;
    size_t channel_count() const;
    /** 轻量 RPC：LoadPlayer(0) 只要通道可达即成功（无资产写） */
    bool Ping(int timeout_ms = 800);

    void ClaimMailAttachmentsAsync(GameDbMailClaimRequest req, MailClaimDone done) override;
    GameDbMailClaimResult ClaimMailAttachments(GameDbMailClaimRequest req) override;

    struct AssetMutationResult {
        bool ok = false;
        bool idempotent_hit = false;
        bool unknown_result = false;
        std::string error_code;
        std::string message;
        uint64_t asset_version = 0;
        uint32_t remain_count = 0;
    };
    bool LoadPlayer(uint64_t player_id, uint64_t *asset_version, bool *exists, std::string *err);
    bool LoadInventory(uint64_t player_id, std::map<uint32_t, uint32_t> *bag, uint64_t *version,
                       std::string *err);
    bool ApplyAssetMutation(uint64_t player_id, const std::string &idempotency_key,
                            uint64_t expected_version, const std::string &mutation_type,
                            uint32_t item_id, uint32_t count, const std::string &trace_id,
                            AssetMutationResult *out);
    bool SavePlayerSnapshot(uint64_t player_id, uint64_t expected_version,
                            const std::map<uint32_t, uint32_t> &bag,
                            const std::string &idempotency_key, uint64_t *new_version,
                            std::string *err);
    bool FlushPlayer(uint64_t player_id, const std::string &reason, uint64_t *version,
                     std::string *err);
    bool QueryOperationResult(uint64_t player_id, const std::string &idempotency_key,
                              const std::string &operation_type, bool *found, bool *completed_ok,
                              bool *idempotent_hit, uint64_t *asset_version, uint32_t *remain_count,
                              std::string *err, std::string *status = nullptr);

private:
    BrpcGameDbRepository() = default;
    std::shared_ptr<const GameDbChannelSnapshot> Current() const;
    void Publish(std::shared_ptr<GameDbChannelSnapshot> next);
    std::shared_ptr<brpc::Channel> ChannelForPlayer(uint64_t player_id);
    std::shared_ptr<brpc::Channel> ChannelAt(size_t idx);
    bool FillMutationFromQuery(uint64_t player_id, const std::string &idempotency_key,
                               const std::string &mutation_type, AssetMutationResult *out);

    std::shared_ptr<const GameDbChannelSnapshot> snap_{std::make_shared<GameDbChannelSnapshot>()};
    std::atomic<uint64_t> version_{0};
    std::atomic<bool> started_{false};
    int timeout_ms_ = 3000;
};
